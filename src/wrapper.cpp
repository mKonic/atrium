#include "wrapper.hpp"

#include "wlr.hpp"

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace atrium {

namespace {

constexpr int kCrashLimit = 3;
constexpr double kCrashWindow = 60;

pid_t child_pid = -1;

void forward(int sig) {
    if (child_pid > 0)
        kill(child_pid, sig);
}

double now_seconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// libwayland's wl_display_add_socket_auto, held here instead: the first
// wayland-N whose lock is free.
struct Socket {
    int fd = -1, lock = -1;
    std::string name, path, lock_path;
};

bool open_socket(Socket& s) {
    const char* dir = std::getenv("XDG_RUNTIME_DIR");
    if (!dir)
        return false;
    for (int n = 0; n < 32; ++n) {
        s.name = "wayland-" + std::to_string(n);
        s.path = std::string(dir) + "/" + s.name;
        s.lock_path = s.path + ".lock";
        s.lock = open(s.lock_path.c_str(), O_CREAT | O_CLOEXEC | O_RDWR, 0640);
        if (s.lock < 0)
            continue;
        if (flock(s.lock, LOCK_EX | LOCK_NB) != 0) {
            close(s.lock);
            continue;
        }
        unlink(s.path.c_str());  // a dead compositor's
        s.fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        if (s.path.size() >= sizeof addr.sun_path || s.fd < 0) {
            close(s.lock);
            return false;
        }
        std::memcpy(addr.sun_path, s.path.c_str(), s.path.size() + 1);
        if (bind(s.fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) == 0 && listen(s.fd, 128) == 0)
            return true;
        wlr_log_errno(WLR_ERROR, "wrapper: can't listen on %s", s.path.c_str());
        close(s.fd);
        close(s.lock);
        s.fd = s.lock = -1;
    }
    return false;
}

} // namespace

bool too_many_crashes(const double* times, int count, double now) {
    int recent = 0;
    for (int i = 0; i < count; ++i)
        if (now - times[i] < kCrashWindow)
            ++recent;
    return recent >= kCrashLimit;
}

int take_wrapped_socket(const char** name) {
    const char* fd = std::getenv("ATRIUM_WAYLAND_SOCKET_FD");
    const char* n = std::getenv("ATRIUM_WAYLAND_SOCKET_NAME");
    if (!fd || !n)
        return -1;
    static std::string kept;
    kept = n;
    *name = kept.c_str();
    const int out = std::atoi(fd);
    unsetenv("ATRIUM_WAYLAND_SOCKET_FD");
    unsetenv("ATRIUM_WAYLAND_SOCKET_NAME");
    // Nothing the compositor starts should get it.
    fcntl(out, F_SETFD, FD_CLOEXEC);
    return out;
}

int run_wrapper(char** argv) {
    Socket s;
    if (!open_socket(s)) {
        wlr_log(WLR_ERROR, "wrapper: no Wayland socket; running without crash recovery");
        return -1;
    }
    wlr_log(WLR_INFO, "wrapper: holding %s", s.name.c_str());
    struct sigaction sa{};
    sa.sa_handler = forward;
    sigemptyset(&sa.sa_mask);
    for (int sig : {SIGTERM, SIGINT, SIGHUP})
        sigaction(sig, &sa, nullptr);

    std::vector<double> crashes;
    int status = 0;
    for (;;) {
        child_pid = fork();
        if (child_pid < 0) {
            wlr_log_errno(WLR_ERROR, "wrapper: fork");
            status = 1;
            break;
        }
        if (child_pid == 0) {
            for (int sig : {SIGTERM, SIGINT, SIGHUP})
                signal(sig, SIG_DFL);
            const int fd = dup(s.fd);  // without CLOEXEC, for the exec
            setenv("ATRIUM_WAYLAND_SOCKET_FD", std::to_string(fd).c_str(), 1);
            setenv("ATRIUM_WAYLAND_SOCKET_NAME", s.name.c_str(), 1);
            if (!crashes.empty())
                setenv("ATRIUM_RESTARTED", "1", 1);
            // By its path, so it's "atrium" to ps and coredumps (an updated
            // binary is the one started).
            char self[4096];
            const ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1);
            std::string path = n > 0 ? std::string(self, size_t(n)) : std::string();
            if (path.ends_with(" (deleted)"))
                path.resize(path.size() - 10);
            if (path.empty() || access(path.c_str(), X_OK) != 0)
                path = "/proc/self/exe";
            execv(path.c_str(), argv);
            _exit(127);
        }
        int st = 0;
        while (waitpid(child_pid, &st, 0) < 0 && errno == EINTR) {
        }
        child_pid = -1;
        const bool crashed = WIFSIGNALED(st) && WTERMSIG(st) != SIGTERM && WTERMSIG(st) != SIGINT &&
                             WTERMSIG(st) != SIGHUP && WTERMSIG(st) != SIGKILL;
        status = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
        if (!crashed)
            break;
        const double now = now_seconds();
        crashes.push_back(now);
        if (too_many_crashes(crashes.data(), int(crashes.size()), now)) {
            wlr_log(WLR_ERROR, "wrapper: the compositor crashed %d times within a minute; giving up", kCrashLimit);
            break;
        }
        wlr_log(WLR_ERROR, "wrapper: the compositor crashed (signal %d); starting it again", WTERMSIG(st));
    }
    close(s.fd);
    unlink(s.path.c_str());
    unlink(s.lock_path.c_str());
    close(s.lock);
    return status;
}

} // namespace atrium
