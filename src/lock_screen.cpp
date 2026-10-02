#include "lock_screen.hpp"

#include "child_watch.hpp"
#include "paths.hpp"
#include "server.hpp"
#include "session_lock.hpp"
#include "shell_process.hpp"

#include <algorithm>
#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <sys/wait.h>
#include <unistd.h>

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
    if (pipe_source_)
        wl_event_source_remove(pipe_source_);
    for (int fd : pipe_)
        if (fd >= 0)
            close(fd);
}

void LockScreen::lock() {
    // Locked by another locker that is still alive, or ours is coming up.
    if (pid_ > 0 || (server_.locked && server_.lock))
        return;
    failures_ = 0;
    start();
}

void LockScreen::start() {
    if (pid_ > 0 || server_.shutting_down)
        return;
    const std::string dir = builtin_dir();
    if (dir.empty()) {
        alog(Log::Error, "lock: no shell to show the lock screen with");
        return;
    }
    const std::string plugins = dev_plugin_dir();
    const bool software = failures_ >= kSoftwareAfter;
    const pid_t pid = fork();
    if (pid < 0) {
        alog_errno(Log::Error, "lock: fork failed");
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
    alog(Log::Info, "lock: lock screen started (pid %d%s)", int(pid), software ? ", software rendering" : "");
}

void LockScreen::exited(int status) {
    pid_ = -1;
    child_watch_set(watch_, -1);
    if (server_.shutting_down)
        return;
    // Unlocked: the password was right.
    if (!server_.locked)
        return;
    ++failures_;
    if (WIFSIGNALED(status))
        alog(Log::Error, "lock: the lock screen died (signal %d); starting it again", WTERMSIG(status));
    else
        alog(Log::Error, "lock: the lock screen exited (%d) with the session locked; starting it again",
             WIFEXITED(status) ? WEXITSTATUS(status) : -1);
    wl_event_source_timer_update(retry_, std::min(5000, 250 * failures_));
}

} // namespace atrium
