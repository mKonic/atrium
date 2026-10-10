#include "daemon.hpp"

#include "worker.hpp"

#ifdef ATRIUM_JOURNAL
#include <systemd/sd-bus.h>
#endif

#include <algorithm>
#include <cerrno>
#include <cstdarg>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <ftw.h>
#include <dirent.h>
#include <linux/kd.h>
#include <linux/vt.h>
#include <poll.h>
#include <pwd.h>
#include <sys/ioctl.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

namespace atrium::login {

using nlohmann::json;

namespace {

constexpr auto kGreeterGrace = std::chrono::seconds(5);   // to quit after starting a session
// GDM's REGISTER_DISPLAY_TIMEOUT: Plymouth goes regardless after this long.
constexpr auto kPlymouthWait = std::chrono::seconds(10);
// DRM_IOCTL_DROP_MASTER, without libdrm.
constexpr unsigned long kDropMaster = _IO('d', 0x1f);
constexpr auto kCrashWindow = std::chrono::seconds(30);   // this many greeter starts in it...
constexpr size_t kCrashLimit = 5;
constexpr auto kCrashPause = std::chrono::seconds(10);    // ...and it waits this long
constexpr size_t kControlClients = 8;
constexpr const char* kControlPath = "/run/atrium-login/control.sock";

__attribute__((format(printf, 1, 2))) void say(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    std::vfprintf(stderr, fmt, args);
    va_end(args);
    std::fputc('\n', stderr);
}

int listen_on(const char* path, mode_t mode, int backlog) {
    unlink(path);
    const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path, sizeof addr.sun_path - 1);
    if (fd < 0 || bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0 || listen(fd, backlog) != 0) {
        say("can't listen on %s: %s", path, strerror(errno));
        if (fd >= 0)
            close(fd);
        return -1;
    }
    if (chmod(path, mode) != 0)
        say("can't chmod %s: %s", path, strerror(errno));
    return fd;
}

} // namespace

Daemon::~Daemon() {
    end(greeter_);
    end(pending_);
    for (auto& s : sessions_)
        end(s);
    drop_client();
    for (auto& c : controls_)
        close(c.fd);
    if (listen_fd_ >= 0) {
        close(listen_fd_);
        unlink(socket_path_.c_str());
    }
    if (control_fd_ >= 0) {
        close(control_fd_);
        unlink(kControlPath);
    }
    if (signal_fd_ >= 0)
        close(signal_fd_);
    for (int fd : display_fds_)
        close(fd);
}

// Closing the last handle on a card makes the kernel restore its console
// (fbdev), which is a flash of text between the splash, the greeter and the
// session. Held here, the screen keeps the last frame (wlroots leaves it
// with CLOSEFB) until the next compositor draws.
void Daemon::hold_displays() {
    DIR* dir = opendir("/dev/dri");
    if (!dir)
        return;
    while (const dirent* e = readdir(dir)) {
        if (std::string_view(e->d_name).substr(0, 4) != "card")
            continue;
        const int fd = open(("/dev/dri/" + std::string(e->d_name)).c_str(), O_RDWR | O_CLOEXEC);
        if (fd < 0)
            continue;
        // The first to open a card becomes its master; the greeter must be.
        ioctl(fd, kDropMaster, 0);
        display_fds_.push_back(fd);
    }
    closedir(dir);
}

// GDM's: `plymouth quit --retain-splash` once something has drawn over the
// splash, a plain quit when nothing will.
void Daemon::quit_plymouth(bool keep_splash) {
    if (!plymouth_)
        return;
    plymouth_ = false;
    plymouth_deadline_.reset();
    const pid_t pid = fork();
    if (pid == 0) {
        sigset_t none;
        sigemptyset(&none);
        sigprocmask(SIG_SETMASK, &none, nullptr);
        if (keep_splash)
            execlp("plymouth", "plymouth", "quit", "--retain-splash", nullptr);
        else
            execlp("plymouth", "plymouth", "quit", nullptr);
        _exit(127);
    }
}

// A session's displays, for the greeter to come up the same way (its home:
// displays.json, read by `atrium --greeter`).
bool Daemon::save_greeter_displays(const std::string& json) {
    const passwd* pw = getpwnam(config_.greeter_user.c_str());
    if (!pw || !pw->pw_dir || !*pw->pw_dir)
        return false;
    const std::string path = std::string(pw->pw_dir) + "/displays.json";
    const std::string tmp = path + ".new";
    const int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0644);
    if (fd < 0)
        return false;
    const bool ok = write(fd, json.data(), json.size()) == ssize_t(json.size()) && fchown(fd, pw->pw_uid, pw->pw_gid) == 0;
    close(fd);
    if (!ok || rename(tmp.c_str(), path.c_str()) != 0) {
        unlink(tmp.c_str());
        return false;
    }
    return true;
}

std::vector<int> Daemon::session_vts() const {
    std::vector<int> vts;
    for (const auto& s : sessions_)
        vts.push_back(s->vt);
    return vts;
}

Daemon::WorkerPtr Daemon::spawn(const std::string& service, const std::string& user, const std::string& cls,
                                bool conversation, int vt) {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv) != 0) {
        say("socketpair failed: %s", strerror(errno));
        return nullptr;
    }
    const pid_t pid = fork();
    if (pid < 0) {
        say("fork failed: %s", strerror(errno));
        close(sv[0]);
        close(sv[1]);
        return nullptr;
    }
    if (pid == 0) {
        close(sv[0]);
        sigset_t none;
        sigemptyset(&none);
        sigprocmask(SIG_SETMASK, &none, nullptr);
        run_worker(sv[1]);
    }
    close(sv[1]);
    auto w = std::make_unique<Worker>();
    w->pid = pid;
    w->fd = sv[0];
    w->vt = vt;
    w->user = user;
    if (const passwd* pw = getpwnam(user.c_str()))
        w->uid = pw->pw_uid;
    const std::string tty = "/dev/tty" + std::to_string(vt);
    w->tty = open(tty.c_str(), O_RDWR | O_NOCTTY | O_CLOEXEC);
    send_message(w->fd, {{"t", "init"}, {"service", service}, {"user", user}, {"class", cls}, {"vt", vt},
                         {"auth", conversation}});
    return w;
}

void Daemon::start(Worker& w, const std::vector<std::string>& cmd, const std::vector<std::string>& env,
                   bool profile) {
    // In graphics mode before it's in front, so the kernel doesn't draw the
    // console over what's on screen until the compositor takes the VT.
    if (w.tty >= 0 && ioctl(w.tty, KDSETMODE, KD_GRAPHICS) != 0)
        say("can't set VT %d to graphics: %s", w.vt, strerror(errno));
    activate_vt(w.vt);
    send_message(w.fd, {{"t", "start"}, {"cmd", cmd}, {"env", env}, {"profile", profile}});
    w.running = true;
}

// Its process ends on its own (an unanswered question fails, a running
// session is stopped) once its socket closes.
void Daemon::end(WorkerPtr& w) {
    if (!w)
        return;
    close(w->fd);
    if (w->tty >= 0) {
        // Back to text for a getty or a console later; only out of sight.
        if (active_vt() != w->vt)
            ioctl(w->tty, KDSETMODE, KD_TEXT);
        close(w->tty);
    }
    w.reset();
}

void Daemon::activate_vt(int vt) {
    const std::string tty = "/dev/tty" + std::to_string(vt);
    const int fd = open(tty.c_str(), O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (fd < 0) {
        say("can't open %s: %s", tty.c_str(), strerror(errno));
        return;
    }
    if (ioctl(fd, VT_ACTIVATE, vt) != 0)
        say("can't switch to VT %d: %s", vt, strerror(errno));
    close(fd);
}

int Daemon::active_vt() {
    const int fd = open("/dev/tty0", O_RDWR | O_NOCTTY | O_CLOEXEC);
    vt_stat st{};
    const bool ok = fd >= 0 && ioctl(fd, VT_GETSTATE, &st) == 0;
    if (fd >= 0)
        close(fd);
    return ok ? st.v_active : 0;
}

int Daemon::free_vt() {
    const int fd = open("/dev/tty0", O_RDWR | O_NOCTTY | O_CLOEXEC);
    int vt = 0;
    if (fd < 0 || ioctl(fd, VT_OPENQRY, &vt) != 0 || vt < 1)
        vt = 0;
    if (fd >= 0)
        close(fd);
    return vt;
}

Daemon::Worker* Daemon::session_of(const std::string& user) {
    for (auto& s : sessions_)
        if (s->user == user && s->running)
            return s.get();
    return nullptr;
}

// Back to a running session: logind brings it to the front (its VT) and
// unlocks it, the greeter having just checked the password.
void Daemon::switch_to(Worker& session) {
    bool done = false;
#ifdef ATRIUM_JOURNAL
    sd_bus* bus = nullptr;
    if (!session.session_id.empty() && sd_bus_open_system(&bus) >= 0) {
        auto call = [&](const char* method) {
            sd_bus_error err = SD_BUS_ERROR_NULL;
            const bool ok = sd_bus_call_method(bus, "org.freedesktop.login1", "/org/freedesktop/login1",
                                               "org.freedesktop.login1.Manager", method, &err, nullptr, "s",
                                               session.session_id.c_str()) >= 0;
            if (!ok)
                say("logind: %s %s: %s", method, session.session_id.c_str(), err.message);
            sd_bus_error_free(&err);
            return ok;
        };
        done = call("ActivateSession");
        call("UnlockSession");
        sd_bus_unref(bus);
    }
#endif
    if (!done)
        activate_vt(session.vt);
}

bool Daemon::open_greeter_socket(uid_t uid, gid_t gid) {
    if (listen_fd_ >= 0)
        return true;
    mkdir("/run/atrium-login", 0711);
    socket_path_ = "/run/atrium-login/greeter.sock";
    listen_fd_ = listen_on(socket_path_.c_str(), 0600, 4);
    if (listen_fd_ < 0)
        return false;
    // Only the greeter's user may log people in.
    if (chown(socket_path_.c_str(), uid, gid) != 0)
        say("can't hand %s to the greeter: %s", socket_path_.c_str(), strerror(errno));
    return true;
}

// Anyone may connect; only a logged-in user (or root) is listened to.
bool Daemon::open_control_socket() {
    mkdir("/run/atrium-login", 0711);
    control_fd_ = listen_on(kControlPath, 0666, 8);
    return control_fd_ >= 0;
}

// The greeter's home (its memory of the last login) is its own, also when
// greetd's setup made it for greetd's user first.
void Daemon::own_home(const passwd& pw) {
    const std::string home = pw.pw_dir ? pw.pw_dir : "";
    if (!home.starts_with("/var/lib/"))
        return;
    struct stat st{};
    if (stat(home.c_str(), &st) != 0) {
        if (mkdir(home.c_str(), 0750) != 0)
            return;
    } else if (st.st_uid == pw.pw_uid) {
        return;
    }
    say("handing %s to %s", home.c_str(), pw.pw_name);
    static uid_t uid;
    static gid_t gid;
    uid = pw.pw_uid;
    gid = pw.pw_gid;
    nftw(home.c_str(), [](const char* path, const struct stat*, int, FTW*) {
        if (lchown(path, uid, gid) != 0)
            say("can't hand %s over: %s", path, strerror(errno));
        return 0;
    }, 16, FTW_PHYS);
}

// On `vt`, or (0) the configured VT while it's free, else a free one.
void Daemon::start_greeter(int vt) {
    if (quitting_ || greeter_)
        return;
    const auto now = Clock::now();
    std::erase_if(greeter_starts_, [&](auto t) { return now - t > kCrashWindow; });
    if (greeter_starts_.size() >= kCrashLimit) {
        say("the greeter keeps failing; trying again in %lld s", (long long)kCrashPause.count());
        greeter_starts_.clear();
        greeter_restart_ = now + kCrashPause;
        return;
    }
    greeter_starts_.push_back(now);
    const passwd* pw = getpwnam(config_.greeter_user.c_str());
    if (!pw) {
        say("no greeter user %s (see sysusers.d/atrium-login.conf)", config_.greeter_user.c_str());
        greeter_restart_ = now + kCrashPause;
        return;
    }
    if (!open_greeter_socket(pw->pw_uid, pw->pw_gid))
        return;
    if (vt == 0) {
        std::vector<int> taken;
        for (const auto& s : sessions_)
            taken.push_back(s->vt);
        vt = greeter_vt(config_.vt, taken, free_vt());
    }
    if (vt == 0) {
        say("no free VT for the greeter");
        return;
    }
    own_home(*pw);
    greeter_ = spawn("atrium-greeter", config_.greeter_user, "greeter", false, vt);
}

// A greeter gone without starting anything: back to the session it came up
// beside, or (nobody logged in) a greeter again.
void Daemon::back_from_greeter() {
    if (quitting_)
        return;
    if (sessions_.empty()) {
        start_greeter();
        return;
    }
    Worker* to = sessions_.back().get();
    for (auto& s : sessions_)
        if (s->vt == return_vt_)
            to = s.get();
    return_vt_ = 0;
    activate_vt(to->vt);  // still locked: its own lock screen asks
}

// The session asked for, once the greeter is gone.
void Daemon::start_pending() {
    const bool waits_for_greeter = greeter_ && pending_ && pending_->vt == greeter_->vt;
    if (!to_start_ || waits_for_greeter || !pending_ || !pending_->authenticated) {
        if (!greeter_ && to_start_ && !pending_) {
            to_start_.reset();
            back_from_greeter();
        }
        return;
    }
    auto [cmd, env] = std::move(*to_start_);
    to_start_.reset();
    return_vt_ = 0;
    // One session a person: logging in again goes back to theirs.
    if (Worker* existing = session_of(pending_->user)) {
        end(pending_);
        switch_to(*existing);
        return;
    }
    sessions_.push_back(std::move(pending_));
    started_beside_ = greeter_ != nullptr;
    start(*sessions_.back(), cmd, env, config_.source_profile);
}

void Daemon::on_worker(WorkerPtr& w) {
    std::optional<json> m = receive_message(w->fd);
    if (!m) {
        on_worker_gone(w);
        return;
    }
    const std::string t = (*m)["t"];
    const bool is_pending = &w == &pending_;
    if (t == "ask" && is_pending) {
        const std::string kind = m->value("kind", "info");
        const AuthKind k = kind == "secret"    ? AuthKind::Secret
                           : kind == "visible" ? AuthKind::Visible
                           : kind == "error"   ? AuthKind::Error
                                               : AuthKind::Info;
        w->asking = true;
        reply(auth_message(k, m->value("text", "")));
    } else if (t == "auth") {
        if (!m->value("ok", false)) {
            say("%s: %s", w->user.c_str(), m->value("error", "").c_str());
            if (is_pending) {
                reply(error(m->value("wrong", false), m->value("error", "")));
                end(w);
            } else {
                on_worker_gone(w);  // couldn't start: show the greeter (again)
            }
            return;
        }
        w->authenticated = true;
        if (&w == &greeter_) {
            std::vector<std::string> env = {"GREETD_SOCK=" + socket_path_};
            // Beside running sessions: it may close and go back.
            if (return_vt_)
                env.push_back("ATRIUM_GREETER_SWITCH=1");
            start(*w, split_command(config_.greeter_command), env, false);
        } else if (is_pending) {
            reply(success());
        } else if (!w->running) {
            start(*w, split_command(config_.autologin_command), {}, config_.source_profile);
        }
    } else if (t == "started") {
        w->session_id = m->value("session", "");
        // An autologin session over the splash: Plymouth goes once it has
        // surely drawn (GDM's wait for a display that registers).
        if (plymouth_ && &w != &greeter_ && !plymouth_deadline_)
            plymouth_deadline_ = Clock::now() + kPlymouthWait;
    } else if (t == "done") {
        on_worker_gone(w);
    }
}

void Daemon::on_worker_gone(WorkerPtr& w) {
    if (&w == &pending_) {
        if (client_fd_ >= 0 && !w->authenticated)
            reply(error(false, "the login stopped"));
        end(w);
        if (to_start_) {
            to_start_.reset();
            if (!greeter_)
                back_from_greeter();
        }
    } else if (&w == &greeter_) {
        end(w);
        greeter_deadline_.reset();
        drop_client();
        quit_plymouth(false);  // it never came up
        if (to_start_)
            start_pending();
        else if (!started_beside_)
            back_from_greeter();
        started_beside_ = false;
    } else {
        for (size_t i = 0; i < sessions_.size(); ++i)
            if (&sessions_[i] == &w) {
                on_session_gone(i);
                return;
            }
    }
}

void Daemon::on_session_gone(size_t index) {
    const int vt = sessions_[index]->vt;
    const bool in_front = active_vt() == vt;
    end(sessions_[index]);
    sessions_.erase(sessions_.begin() + ptrdiff_t(index));
    if (quitting_)
        return;
    if (return_vt_ == vt)
        return_vt_ = sessions_.empty() ? 0 : sessions_.back()->vt;
    if (greeter_)
        return;
    if (sessions_.empty()) {
        start_greeter();
    } else if (in_front) {
        // Logged out with others still in: the greeter where this one was,
        // able to go back to them.
        return_vt_ = sessions_.back()->vt;
        start_greeter(vt);
    }
}

void Daemon::switch_to_greeter(uid_t asker) {
    if (greeter_) {
        activate_vt(greeter_->vt);
        return;
    }
    return_vt_ = active_vt();
    for (auto& s : sessions_)
        if (s->uid == asker && s->running) {
            return_vt_ = s->vt;
            break;
        }
    start_greeter();
}

void Daemon::on_control(size_t index) {
    ControlClient& c = controls_[index];
    char buf[256];
    bool gone = false;
    for (;;) {
        const ssize_t n = read(c.fd, buf, sizeof buf);
        if (n > 0) {
            c.buffer.append(buf, size_t(n));
            if (c.buffer.size() > kControlLineMax)
                gone = true;
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        if (n == 0 || errno != EAGAIN)
            gone = true;
        break;
    }
    const size_t nl = c.buffer.find('\n');
    if (nl != std::string::npos) {
        std::string answer;
        ucred cred{};
        socklen_t len = sizeof cred;
        const bool known = getsockopt(c.fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) == 0;
        const bool allowed = known && (cred.uid == 0 || std::ranges::any_of(sessions_, [&](const WorkerPtr& s) {
                                           return s->running && s->uid == cred.uid;
                                       }));
        if (!allowed) {
            answer = "error: not logged in here\n";
        } else if (auto r = parse_control(std::string_view(c.buffer).substr(0, nl)); !r) {
            answer = "error: unknown request\n";
        } else if (r->kind == Control::Logout) {
            // A greeter of its own (no way back): it comes to the front on
            // a VT already in graphics, then the session ends behind it.
            if (!greeter_) {
                return_vt_ = 0;
                start_greeter();
            }
            answer = "ok\n";
        } else if (r->kind == Control::Displays) {
            answer = save_greeter_displays(r->payload) ? "ok\n" : "error: couldn't keep them\n";
        } else {
            switch_to_greeter(cred.uid);
            answer = "ok\n";
        }
        if (send(c.fd, answer.data(), answer.size(), MSG_NOSIGNAL | MSG_DONTWAIT) < 0)
            say("control: no answer sent: %s", strerror(errno));
        gone = true;
    }
    if (gone) {
        close(c.fd);
        controls_.erase(controls_.begin() + ptrdiff_t(index));
    }
}

void Daemon::drop_client() {
    if (client_fd_ >= 0)
        close(client_fd_);
    client_fd_ = -1;
    client_buffer_.clear();
}

void Daemon::reply(const std::string& body) {
    if (client_fd_ < 0)
        return;
    const std::string out = frame(body);
    size_t done = 0;
    while (done < out.size()) {
        const ssize_t n = send(client_fd_, out.data() + done, out.size() - done, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && errno == EAGAIN) {
            pollfd p{client_fd_, POLLOUT, 0};
            if (poll(&p, 1, 1000) > 0)
                continue;
        }
        if (n <= 0) {
            drop_client();
            return;
        }
        done += size_t(n);
    }
}

void Daemon::on_client() {
    char buf[4096];
    for (;;) {
        const ssize_t n = read(client_fd_, buf, sizeof buf);
        if (n > 0) {
            client_buffer_.append(buf, size_t(n));
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        if (n == 0 || errno != EAGAIN) {
            drop_client();
            // A half-made login goes with its greeter; one it started stays
            // (it quits right after asking).
            if (!to_start_)
                end(pending_);
            return;
        }
        break;
    }
    bool bad = false;
    for (const std::string& body : unframe(client_buffer_, &bad)) {
        if (std::optional<Request> r = parse_request(body))
            handle(*r);
        else
            reply(error(false, "malformed request"));
        if (client_fd_ < 0)
            return;
    }
    if (bad)
        drop_client();
}

void Daemon::handle(const Request& r) {
    switch (r.type) {
    case Request::Type::CreateSession:
        if (pending_ || to_start_) {
            reply(error(false, "a session is already being made"));
            return;
        }
        if (!getpwnam(r.username.c_str())) {
            reply(error(true, "no such user"));
            return;
        }
        pending_ = spawn("atrium-login", r.username, "user", true,
                         session_vt(greeter_ ? greeter_->vt : config_.vt, session_vts(), free_vt()));
        if (!pending_)
            reply(error(false, "couldn't start the login"));
        return;
    case Request::Type::PostAuthResponse:
        if (!pending_ || !pending_->asking) {
            reply(error(false, "nothing was asked"));
            return;
        }
        pending_->asking = false;
        send_message(pending_->fd, r.response ? json{{"t", "answer"}, {"text", *r.response}} : json{{"t", "answer"}});
        return;
    case Request::Type::StartSession:
        if (!pending_ || !pending_->authenticated || to_start_) {
            reply(error(false, "no session is ready to start"));
            return;
        }
        to_start_.emplace(r.cmd, r.env);
        reply(success());
        // greetd's way: the greeter quits on its own; past the grace it's stopped.
        greeter_deadline_ = Clock::now() + kGreeterGrace;
        start_pending();
        return;
    case Request::Type::CancelSession:
        end(pending_);
        to_start_.reset();
        reply(success());
        return;
    }
}

int Daemon::run() {
    sigset_t mask;
    sigemptyset(&mask);
    for (int sig : {SIGCHLD, SIGTERM, SIGINT, SIGHUP})
        sigaddset(&mask, sig);
    sigprocmask(SIG_BLOCK, &mask, nullptr);
    signal(SIGPIPE, SIG_IGN);
    signal_fd_ = signalfd(-1, &mask, SFD_CLOEXEC | SFD_NONBLOCK);
    open_control_socket();

    // Once a boot: logging out, or this daemon restarting, shows the greeter.
    constexpr const char* kAutologinDone = "/run/atrium-login/autologin-done";
    mkdir("/run/atrium-login", 0711);
    hold_displays();
    // GDM's: the splash stays up, Plymouth letting go of the screen, until
    // the greeter has drawn over it.
    plymouth_ = system("plymouth --ping >/dev/null 2>&1") == 0;
    if (plymouth_) {
        if (system("plymouth deactivate") != 0)
            say("couldn't deactivate plymouth");
        plymouth_deadline_ = Clock::now() + kPlymouthWait;
    }
    if (!config_.autologin_user.empty() && access(kAutologinDone, F_OK) != 0 &&
        getpwnam(config_.autologin_user.c_str())) {
        close(open(kAutologinDone, O_WRONLY | O_CREAT | O_CLOEXEC, 0600));
        say("logging %s in", config_.autologin_user.c_str());
        if (WorkerPtr w = spawn("atrium-autologin", config_.autologin_user, "user", false, config_.vt))
            sessions_.push_back(std::move(w));
    }
    if (sessions_.empty())
        start_greeter();

    while (!quitting_ || greeter_ || !sessions_.empty()) {
        // What each pollfd is: the fixed ones, then the sessions, then the
        // control clients.
        enum { kSignal, kGreeter, kPending, kListen, kClient, kControl, kFixed };
        std::vector<pollfd> fds = {{signal_fd_, POLLIN, 0},
                                   {greeter_ ? greeter_->fd : -1, POLLIN, 0},
                                   {pending_ ? pending_->fd : -1, POLLIN, 0},
                                   {listen_fd_, POLLIN, 0},
                                   {client_fd_, POLLIN, 0},
                                   {control_fd_, POLLIN, 0}};
        for (const auto& s : sessions_)
            fds.push_back({s->fd, POLLIN, 0});
        for (const auto& c : controls_)
            fds.push_back({c.fd, POLLIN, 0});
        int timeout = -1;
        const auto now = Clock::now();
        for (const auto& d : {greeter_deadline_, greeter_restart_, plymouth_deadline_})
            if (d) {
                const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(*d - now).count();
                timeout = timeout < 0 ? int(std::max<long long>(ms, 0)) : std::min(timeout, int(std::max<long long>(ms, 0)));
            }
        if (poll(fds.data(), fds.size(), timeout) < 0 && errno != EINTR) {
            say("poll failed: %s", strerror(errno));
            return 1;
        }
        if (greeter_deadline_ && Clock::now() >= *greeter_deadline_) {
            greeter_deadline_.reset();
            if (greeter_) {
                say("the greeter didn't quit; stopping it");
                send_message(greeter_->fd, {{"t", "stop"}});
            }
        }
        if (plymouth_deadline_ && Clock::now() >= *plymouth_deadline_)
            quit_plymouth(!sessions_.empty());  // a session has drawn; a greeter that never connected hasn't
        if (greeter_restart_ && Clock::now() >= *greeter_restart_) {
            greeter_restart_.reset();
            if (sessions_.empty())
                start_greeter();
        }
        if (fds[kSignal].revents) {
            signalfd_siginfo si;
            while (read(signal_fd_, &si, sizeof si) == sizeof si) {
                if (si.ssi_signo == SIGCHLD) {
                    while (waitpid(-1, nullptr, WNOHANG) > 0) {
                    }
                } else if (!quitting_) {
                    say("stopping");
                    quitting_ = true;
                    end(pending_);
                    if (greeter_)
                        send_message(greeter_->fd, {{"t", "stop"}});
                    for (auto& s : sessions_)
                        send_message(s->fd, {{"t", "stop"}});
                }
            }
        }
        if (fds[kGreeter].revents && greeter_ && greeter_->fd == fds[kGreeter].fd)
            on_worker(greeter_);
        if (fds[kPending].revents && pending_ && pending_->fd == fds[kPending].fd)
            on_worker(pending_);
        // Sessions and control clients by their fd: handling one can end others.
        for (size_t i = kFixed; i < fds.size(); ++i) {
            if (!fds[i].revents)
                continue;
            for (auto& s : sessions_)
                if (s && s->fd == fds[i].fd) {
                    on_worker(s);
                    goto next;
                }
            for (size_t c = 0; c < controls_.size(); ++c)
                if (controls_[c].fd == fds[i].fd) {
                    on_control(c);
                    break;
                }
        next:;
        }
        if (fds[kListen].revents && listen_fd_ >= 0) {
            const int c = accept4(listen_fd_, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
            if (c >= 0) {
                // One greeter at a time: a new connection replaces the old.
                drop_client();
                end(pending_);
                to_start_.reset();
                client_fd_ = c;
                quit_plymouth(true);  // the greeter is up
            }
        }
        if (fds[kClient].revents && client_fd_ >= 0 && client_fd_ == fds[kClient].fd)
            on_client();
        if (fds[kControl].revents && control_fd_ >= 0) {
            const int c = accept4(control_fd_, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
            if (c >= 0 && controls_.size() < kControlClients)
                controls_.push_back({c, {}});
            else if (c >= 0)
                close(c);
        }
    }
    return 0;
}

} // namespace atrium::login
