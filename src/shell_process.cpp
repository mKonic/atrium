#include "shell_process.hpp"

#include "paths.hpp"
#include "server.hpp"

#include <csignal>
#include <cstdlib>
#include <ctime>
#include <fcntl.h>
#include <filesystem>
#include <sys/wait.h>
#include <unistd.h>

namespace atrium {

namespace fs = std::filesystem;

namespace {

// Read by the SIGCHLD handler, which may only do async-signal-safe things.
volatile sig_atomic_t g_shell_pid = -1;
int g_report_fd = -1;

double now_ms() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

std::string quoted(const std::string& s) {
    std::string out = "'";
    for (char c : s)
        out += c == '\'' ? std::string("'\\''") : std::string(1, c);
    return out + "'";
}

// atrium's own shell: from the source tree when atrium runs from its build
// directory (so edits show up), else the installed copy.
std::string builtin_dir() {
    if (const char* env = std::getenv("ATRIUM_SHELL_DIR"); env && *env)
        return env;
    const fs::path source = fs::path(ATRIUM_SOURCE_DIR) / "shell";
    const fs::path installed = fs::path(ATRIUM_DATADIR) / "shell";
    std::error_code ec;
    const fs::path exe = fs::read_symlink("/proc/self/exe", ec);
    const bool from_build = !ec && exe.string().starts_with(ATRIUM_SOURCE_DIR);
    if (from_build && fs::exists(source / "shell.qml"))
        return source;
    if (fs::exists(installed / "shell.qml"))
        return installed;
    if (fs::exists(source / "shell.qml"))
        return source;
    return {};
}

// The Atrium QML module: from the build tree when running from it, else the
// installed copy.
std::string qml_import_dir() {
    std::error_code ec;
    const fs::path exe = fs::read_symlink("/proc/self/exe", ec);
    const fs::path built = fs::path(ATRIUM_BUILD_DIR) / "shell" / "plugin";
    if (!ec && exe.string().starts_with(ATRIUM_BUILD_DIR) && fs::exists(built / "Atrium" / "qmldir"))
        return built;
    if (fs::exists(fs::path(ATRIUM_QML_DIR) / "Atrium" / "qmldir"))
        return ATRIUM_QML_DIR;
    return built;
}

} // namespace

// Called from Server's SIGCHLD handler for every reaped child.
void report_child_exit(pid_t pid, int status) {
    if (pid == g_shell_pid && g_report_fd >= 0) {
        [[maybe_unused]] ssize_t n = write(g_report_fd, &status, sizeof status);
    }
}

ShellProcess::ShellProcess(Server& server) : server_(server) {
    if (pipe2(pipe_, O_CLOEXEC | O_NONBLOCK) == 0) {
        g_report_fd = pipe_[1];
        pipe_source_ = wl_event_loop_add_fd(server_.loop, pipe_[0], WL_EVENT_READABLE, [](int fd, uint32_t, void* data) {
            int status = 0;
            while (read(fd, &status, sizeof status) == sizeof status)
                static_cast<ShellProcess*>(data)->exited(status);
            return 0;
        }, this);
    }
    retry_ = wl_event_loop_add_timer(server_.loop, [](void* data) {
        static_cast<ShellProcess*>(data)->start();
        return 0;
    }, this);
}

ShellProcess::~ShellProcess() {
    stop();
    g_report_fd = -1;
    if (retry_)
        wl_event_source_remove(retry_);
    if (pipe_source_)
        wl_event_source_remove(pipe_source_);
    for (int fd : pipe_)
        if (fd >= 0)
            close(fd);
}

std::string ShellProcess::command() const {
    const std::string& setting = server_.config.shell;
    if (setting.empty() || setting == "none")
        return {};
    if (setting != "builtin")
        return setting;
    const std::string dir = builtin_dir();
    if (dir.empty())
        return {};
    // The login screen is its own shell, next to the desktop's.
    if (server_.config.greeter)
        return "exec qs -p " + quoted(dir + "/greeter.qml");
    return "exec qs -p " + quoted(dir);
}

void ShellProcess::start() {
    if (pid_ > 0)
        return;
    const std::string cmd = command();
    if (cmd.empty())
        return;
    const pid_t pid = fork();
    if (pid < 0) {
        wlr_log_errno(WLR_ERROR, "shell: fork failed");
        return;
    }
    const std::string qml = qml_import_dir();
    if (pid == 0) {
        setsid();
        // Where the shell finds its C++ module (`import Atrium`).
        if (!qml.empty())
            setenv("QML_IMPORT_PATH", qml.c_str(), 1);
        struct sigaction sa{};
        sa.sa_handler = SIG_DFL;
        sigemptyset(&sa.sa_mask);
        for (int sig : {SIGCHLD, SIGINT, SIGTERM, SIGPIPE})
            sigaction(sig, &sa, nullptr);
        execl("/bin/sh", "/bin/sh", "-c", cmd.c_str(), nullptr);
        _exit(127);
    }
    pid_ = pid;
    g_shell_pid = pid;
    started_ms_ = now_ms();
    wlr_log(WLR_INFO, "shell: started '%s' (pid %d)", cmd.c_str(), int(pid));
}

void ShellProcess::stop() {
    wl_event_source_timer_update(retry_, 0);
    if (pid_ <= 0)
        return;
    g_shell_pid = -1;  // an exit we asked for is not a crash
    kill(-pid_, SIGTERM);
    pid_ = -1;
}

void ShellProcess::restart() {
    stop();
    quick_failures_ = 0;
    schedule(300);  // give the old one a moment to let go of its surfaces
}

void ShellProcess::schedule(int delay_ms) {
    wl_event_source_timer_update(retry_, delay_ms);
}

void ShellProcess::exited(int status) {
    pid_ = -1;
    g_shell_pid = -1;
    if (server_.shutting_down)
        return;
    // The greeter is done once it has started a session: greetd runs that
    // after the greeter's compositor (us) is gone.
    if (server_.config.greeter && WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        wlr_log(WLR_INFO, "greeter: done, leaving for the session");
        server_.quit();
        return;
    }
    const double lived = now_ms() - started_ms_;
    if (WIFEXITED(status))
        wlr_log(WLR_ERROR, "shell: exited with %d after %.1fs", WEXITSTATUS(status), lived / 1000);
    else if (WIFSIGNALED(status))
        wlr_log(WLR_ERROR, "shell: killed by signal %d after %.1fs", WTERMSIG(status), lived / 1000);

    // Came back fine for a while: start over. Keeps dying at once: wait
    // longer each time, and give up after a handful.
    if (lived > 30000)
        quick_failures_ = 0;
    else
        ++quick_failures_;
    if (quick_failures_ > 5) {
        wlr_log(WLR_ERROR, "shell: keeps failing, not restarting (atriumctl action restart-shell to try again)");
        return;
    }
    schedule(quick_failures_ == 0 ? 500 : std::min(30000, 1000 << (quick_failures_ - 1)));
}

} // namespace atrium
