#include "lock_screen.hpp"

#include "child_watch.hpp"
#include "paths.hpp"
#include "server.hpp"
#include "session_lock.hpp"
#include "shell_process.hpp"
#include "wlr.hpp"

#include <algorithm>
#include <cstring>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#ifdef ATRIUM_JOURNAL  // libsystemd
#include <systemd/sd-bus.h>
#endif
#include <filesystem>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>

namespace atrium {

namespace {

constexpr int kSoftwareAfter = 2;  // failures before it renders in software

// Where Qt finds the atrium-lock integration when atrium runs from its
// build tree (installed, it's in Qt's own plugin directory).
std::string dev_plugin_dir() {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path exe = fs::read_symlink("/proc/self/exe", ec);
    const fs::path dir = fs::path(ATRIUM_BUILD_DIR) / "shell" / "lock" / "plugins";
    if (!ec && exe.string().starts_with(ATRIUM_BUILD_DIR) && fs::exists(dir))
        return dir;
    return {};
}

} // namespace

LockScreen::LockScreen(Server& server) : server_(server) {
    if (pipe2(pipe_, O_CLOEXEC | O_NONBLOCK) == 0) {
        watch_ = child_watch_add(pipe_[1]);
        pipe_source_ = wl_event_loop_add_fd(server_.loop, pipe_[0], WL_EVENT_READABLE, [](int fd, uint32_t, void* data) {
            int status = 0;
            while (read(fd, &status, sizeof status) == sizeof status)
                static_cast<LockScreen*>(data)->exited(status);
            return 0;
        }, this);
    }
    retry_ = wl_event_loop_add_timer(server_.loop, [](void* data) {
        // Still locked and nobody holding the lock (by now the dead one's
        // connection is gone too).
        auto* self = static_cast<LockScreen*>(data);
        if (self->server_.locked && !self->server_.lock)
            self->start();
        return 0;
    }, this);
}

LockScreen::~LockScreen() {
    child_watch_remove(watch_);
    if (pid_ > 0)
        kill(-pid_, SIGTERM);
    if (retry_)
        wl_event_source_remove(retry_);
    if (switch_wait_)
        wl_event_source_remove(switch_wait_);
    if (pipe_source_)
        wl_event_source_remove(pipe_source_);
    for (int fd : pipe_)
        if (fd >= 0)
            close(fd);
}

void LockScreen::lock() {
    // Ours is on its way out (unlocked from outside): a new one once it's gone.
    if (pid_ > 0 && stopping_) {
        relock_ = true;
        return;
    }
    // Locked by another locker that is still alive, or ours is coming up.
    if (pid_ > 0 || (server_.locked && server_.lock))
        return;
    failures_ = 0;
    start();
}

namespace {

constexpr int kSwitchPoll = 50;     // ms between looks at the lock
constexpr int kSwitchTries = 200;   // 10 s for the lock screen to take the lock
constexpr const char* kLoginSocket = "/run/atrium-login/control.sock";

// "switch-to-greeter" on atrium-login's control socket; false if it isn't
// running or won't.
bool ask_atrium_login() {
    const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return false;
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, kLoginSocket, sizeof addr.sun_path - 1);
    timeval tv{2, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    std::string answer;
    if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) == 0 &&
        send(fd, "switch-to-greeter\n", 18, MSG_NOSIGNAL) == 18) {
        char buf[128];
        const ssize_t n = recv(fd, buf, sizeof buf, 0);
        if (n > 0)
            answer.assign(buf, size_t(n));
    }
    close(fd);
    if (answer.starts_with("ok"))
        return true;
    if (!answer.empty())
        wlr_log(WLR_ERROR, "lock: atrium-login won't show a greeter: %s", answer.c_str());
    return false;
}

// The display manager's seat (SDDM, LightDM): its SwitchToGreeter.
bool ask_display_manager() {
#ifdef ATRIUM_JOURNAL
    const char* seat = std::getenv("XDG_SEAT_PATH");
    if (!seat || !*seat)
        return false;
    sd_bus* bus = nullptr;
    if (sd_bus_open_system(&bus) < 0)
        return false;
    sd_bus_error err = SD_BUS_ERROR_NULL;
    const int r = sd_bus_call_method(bus, "org.freedesktop.DisplayManager", seat,
                                     "org.freedesktop.DisplayManager.Seat", "SwitchToGreeter", &err, nullptr, "");
    if (r < 0)
        wlr_log(WLR_ERROR, "lock: the display manager won't show a greeter: %s", err.message ? err.message : "?");
    sd_bus_error_free(&err);
    sd_bus_flush_close_unref(bus);
    return r >= 0;
#else
    return false;
#endif
}

bool ask_for_greeter() {
    if (ask_atrium_login() || ask_display_manager())
        return true;
    wlr_log(WLR_ERROR, "lock: no login manager to show a greeter");
    return false;
}

} // namespace

void LockScreen::unlock() {
    if (!server_.locked)
        return;
    if (server_.lock && pid_ <= 0)
        return;
    if (server_.lock)
        server_.lock->unlock();
    else
        SessionLock::ended(server_, true);
    if (pid_ > 0) {
        stopping_ = true;
        relock_ = false;
        kill(-pid_, SIGTERM);  // a lock screen with nothing to lock
    }
}

void LockScreen::switch_user() {
    if (switch_tries_ > 0)
        return;  // already on its way
    if (!switch_wait_)
        switch_wait_ = wl_event_loop_add_timer(server_.loop, [](void* data) {
            auto* self = static_cast<LockScreen*>(data);
            // Locked, and the lock screen up to say so.
            if (self->server_.locked && self->server_.lock) {
                self->switch_tries_ = 0;
                ask_for_greeter();
            } else if (++self->switch_tries_ >= kSwitchTries) {
                self->switch_tries_ = 0;
                wlr_log(WLR_ERROR, "lock: the session didn't lock; not switching user");
            } else {
                wl_event_source_timer_update(self->switch_wait_, kSwitchPoll);
            }
            return 0;
        }, this);
    lock();
    switch_tries_ = 1;
    wl_event_source_timer_update(switch_wait_, kSwitchPoll);
}

void LockScreen::start() {
    if (pid_ > 0 || server_.shutting_down)
        return;
    const std::string dir = builtin_dir();
    if (dir.empty()) {
        wlr_log(WLR_ERROR, "lock: no shell to show the lock screen with");
        return;
    }
    const std::string plugins = dev_plugin_dir();
    const bool software = failures_ >= kSoftwareAfter;
    const pid_t pid = fork();
    if (pid < 0) {
        wlr_log_errno(WLR_ERROR, "lock: fork failed");
        return;
    }
    if (pid == 0) {
        setsid();
        struct sigaction sa{};
        sa.sa_handler = SIG_DFL;
        sigemptyset(&sa.sa_mask);
        for (int sig : {SIGCHLD, SIGINT, SIGTERM, SIGPIPE})
            sigaction(sig, &sa, nullptr);
        setenv("QT_QPA_PLATFORM", "wayland", 1);
        setenv("QT_WAYLAND_SHELL_INTEGRATION", "atrium-lock", 1);
        if (!plugins.empty()) {
            const char* old = getenv("QT_PLUGIN_PATH");
            setenv("QT_PLUGIN_PATH", (plugins + (old && *old ? ":" + std::string(old) : "")).c_str(), 1);
        }
        if (software) {
            setenv("QT_QUICK_BACKEND", "software", 1);
            setenv("ATRIUM_SAFE_MODE", "1", 1);
        }
        const std::string lock_qml = dir + "/lock.qml";
        const std::string shell = shell_binary();
        execl(shell.c_str(), shell.c_str(), lock_qml.c_str(), nullptr);
        _exit(127);
    }
    pid_ = pid;
    child_watch_set(watch_, pid);
    wlr_log(WLR_INFO, "lock: lock screen started (pid %d%s)", int(pid), software ? ", software rendering" : "");
}

void LockScreen::exited(int status) {
    pid_ = -1;
    stopping_ = false;
    child_watch_set(watch_, -1);
    if (server_.shutting_down)
        return;
    if (std::exchange(relock_, false)) {
        lock();
        return;
    }
    // Unlocked: the password was right.
    if (!server_.locked)
        return;
    ++failures_;
    if (WIFSIGNALED(status))
        wlr_log(WLR_ERROR, "lock: the lock screen died (signal %d); starting it again", WTERMSIG(status));
    else
        wlr_log(WLR_ERROR, "lock: the lock screen exited (%d) with the session locked; starting it again",
             WIFEXITED(status) ? WEXITSTATUS(status) : -1);
    wl_event_source_timer_update(retry_, std::min(5000, 250 * failures_));
}

} // namespace atrium
