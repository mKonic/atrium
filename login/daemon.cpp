#include "daemon.hpp"

#include "worker.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdarg>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <ftw.h>
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
constexpr auto kCrashWindow = std::chrono::seconds(30);   // this many greeter starts in it...
constexpr size_t kCrashLimit = 5;
constexpr auto kCrashPause = std::chrono::seconds(10);    // ...and it waits this long

__attribute__((format(printf, 1, 2))) void say(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    std::vfprintf(stderr, fmt, args);
    va_end(args);
    std::fputc('\n', stderr);
}

} // namespace

Daemon::~Daemon() {
    for (auto* w : {&greeter_, &pending_, &session_})
        if (*w && (*w)->fd >= 0)
            close((*w)->fd);
    drop_client();
    if (listen_fd_ >= 0) {
        close(listen_fd_);
        unlink(socket_path_.c_str());
    }
    if (signal_fd_ >= 0)
        close(signal_fd_);
}

std::unique_ptr<Daemon::Worker> Daemon::spawn(const std::string& service, const std::string& user,
                                              const std::string& cls, bool conversation) {
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
    w->user = user;
    send_message(w->fd, {{"t", "init"}, {"service", service}, {"user", user}, {"class", cls}, {"vt", config_.vt},
                         {"auth", conversation}});
    return w;
}

void Daemon::start(Worker& w, const std::vector<std::string>& cmd, const std::vector<std::string>& env,
                   bool profile) {
    activate_vt();
    send_message(w.fd, {{"t", "start"}, {"cmd", cmd}, {"env", env}, {"profile", profile}});
    w.running = true;
}

// Its process ends on its own (an unanswered question fails, a running
// session is stopped) once its socket closes.
void Daemon::end(std::unique_ptr<Worker>& w) {
    if (!w)
        return;
    close(w->fd);
    w.reset();
}

void Daemon::activate_vt() {
    const std::string tty = "/dev/tty" + std::to_string(config_.vt);
    const int fd = open(tty.c_str(), O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (fd < 0) {
        say("can't open %s: %s", tty.c_str(), strerror(errno));
        return;
    }
    if (ioctl(fd, VT_ACTIVATE, config_.vt) != 0)
        say("can't switch to VT %d: %s", config_.vt, strerror(errno));
    close(fd);
}

bool Daemon::open_greeter_socket(uid_t uid, gid_t gid) {
    if (listen_fd_ >= 0)
        return true;
    mkdir("/run/atrium-login", 0711);
    socket_path_ = "/run/atrium-login/greeter.sock";
    unlink(socket_path_.c_str());
    listen_fd_ = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, socket_path_.c_str(), sizeof addr.sun_path - 1);
    if (listen_fd_ < 0 || bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0 ||
        listen(listen_fd_, 4) != 0) {
        say("can't listen on %s: %s", socket_path_.c_str(), strerror(errno));
        if (listen_fd_ >= 0)
            close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }
    // Only the greeter's user may log people in.
    if (chown(socket_path_.c_str(), uid, gid) != 0 || chmod(socket_path_.c_str(), 0600) != 0)
        say("can't hand %s to the greeter: %s", socket_path_.c_str(), strerror(errno));
    return true;
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

void Daemon::start_greeter() {
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
    own_home(*pw);
    greeter_ = spawn("atrium-greeter", config_.greeter_user, "greeter", false);
}

// The session asked for, once the greeter is gone.
void Daemon::start_pending() {
    if (!to_start_ || greeter_ || !pending_ || !pending_->authenticated) {
        if (!greeter_ && !session_ && to_start_ && !pending_) {
            to_start_.reset();
            start_greeter();
        }
        return;
    }
    auto [cmd, env] = std::move(*to_start_);
    to_start_.reset();
    session_ = std::move(pending_);
    start(*session_, cmd, env, config_.source_profile);
}

void Daemon::on_worker(std::unique_ptr<Worker>& w) {
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
            if (is_pending)
                reply(error(m->value("wrong", false), m->value("error", "")));
            end(w);
            if (&w == &greeter_ || &w == &session_)
                start_greeter();  // couldn't start: show the greeter (again)
            return;
        }
        w->authenticated = true;
        if (&w == &greeter_)
            start(*w, split_command(config_.greeter_command), {"GREETD_SOCK=" + socket_path_}, false);
        else if (is_pending)
            reply(success());
        else if (&w == &session_ && !w->running)
            start(*w, split_command(config_.autologin_command), {}, config_.source_profile);
    } else if (t == "done") {
        on_worker_gone(w);
    }
}

void Daemon::on_worker_gone(std::unique_ptr<Worker>& w) {
    const bool was_greeter = &w == &greeter_, was_session = &w == &session_, was_pending = &w == &pending_;
    if (was_pending && client_fd_ >= 0 && !w->authenticated)
        reply(error(false, "the login stopped"));
    end(w);
    if (was_greeter) {
        greeter_deadline_.reset();
        drop_client();
        if (to_start_)
            start_pending();
        else if (!session_)
            start_greeter();
    } else if (was_session) {
        start_greeter();
    } else if (was_pending && to_start_) {
        to_start_.reset();
        if (!greeter_)
            start_greeter();
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
        pending_ = spawn("atrium-login", r.username, "user", true);
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
        if (!greeter_)
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

    // Once a boot: logging out, or this daemon restarting, shows the greeter.
    constexpr const char* kAutologinDone = "/run/atrium-login/autologin-done";
    mkdir("/run/atrium-login", 0711);
    if (!config_.autologin_user.empty() && access(kAutologinDone, F_OK) != 0 &&
        getpwnam(config_.autologin_user.c_str())) {
        close(open(kAutologinDone, O_WRONLY | O_CREAT | O_CLOEXEC, 0600));
        say("logging %s in", config_.autologin_user.c_str());
        session_ = spawn("atrium-autologin", config_.autologin_user, "user", false);
    }
    if (!session_)
        start_greeter();

    while (!quitting_ || greeter_ || session_) {
        std::vector<pollfd> fds = {{signal_fd_, POLLIN, 0}};
        auto add = [&](int fd) { fds.push_back({fd, POLLIN, 0}); };
        add(greeter_ ? greeter_->fd : -1);
        add(pending_ ? pending_->fd : -1);
        add(session_ ? session_->fd : -1);
        add(listen_fd_);
        add(client_fd_);
        int timeout = -1;
        const auto now = Clock::now();
        for (const auto& d : {greeter_deadline_, greeter_restart_})
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
        if (greeter_restart_ && Clock::now() >= *greeter_restart_) {
            greeter_restart_.reset();
            start_greeter();
        }
        if (fds[0].revents) {
            signalfd_siginfo si;
            while (read(signal_fd_, &si, sizeof si) == sizeof si) {
                if (si.ssi_signo == SIGCHLD) {
                    while (waitpid(-1, nullptr, WNOHANG) > 0) {
                    }
                } else if (!quitting_) {
                    say("stopping");
                    quitting_ = true;
                    end(pending_);
                    for (auto* w : {&greeter_, &session_})
                        if (*w)
                            send_message((*w)->fd, {{"t", "stop"}});
                }
            }
        }
        if (fds[1].revents && greeter_)
            on_worker(greeter_);
        if (fds[2].revents && pending_)
            on_worker(pending_);
        if (fds[3].revents && session_)
            on_worker(session_);
        if (fds[4].revents && listen_fd_ >= 0) {
            const int c = accept4(listen_fd_, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
            if (c >= 0) {
                // One greeter at a time: a new connection replaces the old.
                drop_client();
                end(pending_);
                to_start_.reset();
                client_fd_ = c;
            }
        }
        if (fds[5].revents && client_fd_ >= 0 && client_fd_ == fds[5].fd)
            on_client();
    }
    return 0;
}

} // namespace atrium::login
