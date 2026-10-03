#ifdef ATRIUM_XWAYLAND
#include "xwayland/server.hpp"

#include "util/log.hpp"

#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

namespace atrium::xwayland {

namespace {

constexpr char kSocketDir[] = "/tmp/.X11-unix";

void safe_close(int& fd) {
    if (fd >= 0)
        close(fd);
    fd = -1;
}

bool set_cloexec(int fd, bool on) {
    int flags = fcntl(fd, F_GETFD);
    if (flags < 0)
        return false;
    flags = on ? flags | FD_CLOEXEC : flags & ~FD_CLOEXEC;
    return fcntl(fd, F_SETFD, flags) == 0;
}

std::string lock_path(int display) { return "/tmp/.X" + std::to_string(display) + "-lock"; }
std::string socket_path(int display) { return std::string(kSocketDir) + "/X" + std::to_string(display); }

// A listening socket at `path` (abstract if it starts with a NUL).
int open_socket(const std::string& path) {
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof addr.sun_path)
        return -1;
    path.copy(addr.sun_path, path.size());
    const auto size = socklen_t(offsetof(sockaddr_un, sun_path) + path.size());
    const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        alog_errno(Log::Error, "xwayland: can't make a socket");
        return -1;
    }
    if (path[0])
        unlink(path.c_str());
    if (bind(fd, reinterpret_cast<sockaddr*>(&addr), size) < 0 || listen(fd, 1) < 0) {
        const int err = errno;
        alog_errno(Log::Error, "xwayland: can't listen on %c%s", path[0] ? path[0] : '@', path.c_str() + 1);
        close(fd);
        if (path[0])
            unlink(path.c_str());
        errno = err;
        return -1;
    }
    return fd;
}

bool check_socket_dir() {
    struct stat st{};
    if (lstat(kSocketDir, &st)) {
        alog_errno(Log::Error, "xwayland: can't stat %s", kSocketDir);
        return false;
    }
    if (!S_ISDIR(st.st_mode)) {
        alog(Log::Error, "xwayland: %s is not a directory", kSocketDir);
        return false;
    }
    if (st.st_uid != 0 && st.st_uid != getuid()) {
        alog(Log::Error, "xwayland: %s is not owned by root or us", kSocketDir);
        return false;
    }
    // Without the sticky bit, only if no one else can write there.
    if (!(st.st_mode & S_ISVTX) && (st.st_mode & (S_IWGRP | S_IWOTH))) {
        alog(Log::Error, "xwayland: %s has no sticky bit", kSocketDir);
        return false;
    }
    return true;
}

bool open_sockets(int socks[2], int display) {
    if (mkdir(kSocketDir, 0755) == 0) {
        alog(Log::Info, "xwayland: made %s (other users can't make X sockets there)", kSocketDir);
    } else if (errno != EEXIST) {
        alog_errno(Log::Error, "xwayland: can't make %s", kSocketDir);
        return false;
    } else if (!check_socket_dir()) {
        return false;
    }
    const std::string path = socket_path(display);
    socks[0] = open_socket(std::string(1, '\0') + path);
    if (socks[0] < 0)
        return false;
    socks[1] = open_socket(path);
    if (socks[1] < 0) {
        safe_close(socks[0]);
        return false;
    }
    return true;
}

} // namespace

int open_display_sockets(int socks[2]) {
    for (int display = 0; display <= 32; ++display) {
        const std::string lock = lock_path(display);
        if (int fd = open(lock.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0444); fd >= 0) {
            char pid[12];
            snprintf(pid, sizeof pid, "%10d", getpid());
            const bool ok = open_sockets(socks, display);
            if (ok && write(fd, pid, sizeof pid - 1) == ssize_t(sizeof pid - 1)) {
                close(fd);
                return display;
            }
            if (ok) {
                safe_close(socks[0]);
                safe_close(socks[1]);
                unlink(socket_path(display).c_str());
            }
            unlink(lock.c_str());
            close(fd);
            continue;
        }
        // Taken: by a live process?
        const int fd = open(lock.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0)
            continue;
        char pid[12] = {};
        const ssize_t n = read(fd, pid, sizeof pid - 1);
        close(fd);
        if (n != ssize_t(sizeof pid - 1))
            continue;
        char* end = nullptr;
        const long owner = strtol(pid, &end, 10);
        if (owner < 0 || owner > INT32_MAX || end != pid + sizeof pid - 2)
            continue;
        errno = 0;
        if (kill(pid_t(owner), 0) != 0 && errno == ESRCH && unlink(lock.c_str()) == 0)
            --display;  // a stale lock: try this one again
    }
    alog(Log::Error, "xwayland: none of the displays :0 to :32 is free");
    return -1;
}

void unlink_display_sockets(int display) {
    unlink(socket_path(display).c_str());
    unlink(lock_path(display).c_str());
}

Server::Server(wl_display* display) : wl_display_(display) {
    const char* path = getenv("ATRIUM_XWAYLAND");
    if (!path && access(ATRIUM_XWAYLAND_PATH, X_OK) != 0) {
        alog(Log::Error, "xwayland: no Xwayland at %s", ATRIUM_XWAYLAND_PATH);
        return;
    }
    // The wrapper's (wrapper.hpp): the same DISPLAY across compositor restarts.
    if (const char* held = getenv("ATRIUM_X11_DISPLAY"), *fds = getenv("ATRIUM_X11_FDS"); held && fds &&
        sscanf(fds, "%d,%d", &x_fd_[0], &x_fd_[1]) == 2) {
        display_ = atoi(held);
        held_ = true;
        set_cloexec(x_fd_[0], true);
        set_cloexec(x_fd_[1], true);
    } else {
        display_ = open_display_sockets(x_fd_);
    }
    unsetenv("ATRIUM_X11_DISPLAY");
    unsetenv("ATRIUM_X11_FDS");
    if (display_ < 0)
        return;
    display_name_ = ":" + std::to_string(display_);
    idle_ = wl_event_loop_add_idle(
        wl_display_get_event_loop(display),
        [](void* data) {
            auto* s = static_cast<Server*>(data);
            s->idle_ = nullptr;
            s->start();
        },
        this);
}

Server::~Server() {
    if (idle_)
        wl_event_source_remove(idle_);
    finish_process();
    finish_display();
}

bool Server::start() {
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, wl_fd_) != 0 ||
        socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, wm_fd_) != 0) {
        alog_errno(Log::Error, "xwayland: socketpair failed");
        finish_process();
        return false;
    }
    started_ = time(nullptr);
    client_ = wl_client_create(wl_display_, wl_fd_[0]);
    if (!client_) {
        alog_errno(Log::Error, "xwayland: wl_client_create failed");
        finish_process();
        return false;
    }
    wl_fd_[0] = -1;  // the client's now
    client_destroy_.notify = [](wl_listener* l, void*) {
        Server* s = wl_container_of(l, s, client_destroy_);
        if (s->pipe_source_)
            return;  // it failed to start: on_ready sees to it
        // It's being destroyed already.
        s->client_ = nullptr;
        wl_list_remove(&s->client_destroy_.link);
        s->finish_process();
        // Again, unless it died straight away (it would again).
        if (time(nullptr) - s->started_ > 5) {
            alog(Log::Info, "xwayland: restarting Xwayland");
            s->start();
        }
    };
    wl_client_add_destroy_listener(client_, &client_destroy_);

    int notify[2];
    if (pipe2(notify, O_CLOEXEC) != 0) {
        alog_errno(Log::Error, "xwayland: pipe failed");
        finish_process();
        return false;
    }
    pipe_source_ = wl_event_loop_add_fd(wl_display_get_event_loop(wl_display_), notify[0], WL_EVENT_READABLE,
                                        on_ready, this);
    events.start.emit();

    pid_ = fork();
    if (pid_ < 0) {
        alog_errno(Log::Error, "xwayland: fork failed");
        close(notify[0]);
        close(notify[1]);
        finish_process();
        return false;
    }
    if (pid_ == 0) {
        // Twice, so it isn't our child to reap when it exits.
        const pid_t pid = fork();
        if (pid == 0)
            exec(notify[1]);
        _exit(pid < 0 ? EXIT_FAILURE : EXIT_SUCCESS);
    }
    close(notify[1]);
    safe_close(wl_fd_[1]);
    safe_close(wm_fd_[1]);
    return true;
}

void Server::exec(int notify_fd) {
    if (!set_cloexec(x_fd_[0], false) || !set_cloexec(x_fd_[1], false) || !set_cloexec(wl_fd_[1], false) ||
        !set_cloexec(wm_fd_[1], false) || !set_cloexec(notify_fd, false))
        _exit(EXIT_FAILURE);
    const std::string listen0 = std::to_string(x_fd_[0]), listen1 = std::to_string(x_fd_[1]),
                      displayfd = std::to_string(notify_fd), wm = std::to_string(wm_fd_[1]);
    const char* argv[] = {"Xwayland",        display_name_.c_str(), "-rootless", "-core",
                          "-terminate",      "-listenfd",           listen0.c_str(), "-listenfd",
                          listen1.c_str(),   "-displayfd",          displayfd.c_str(), "-wm",
                          wm.c_str(),        nullptr};
    setenv("WAYLAND_SOCKET", std::to_string(wl_fd_[1]).c_str(), 1);
    alog(Log::Info, "xwayland: starting Xwayland on %s", display_name_.c_str());
    // Its output only as far as ours goes.
    const int devnull = open("/dev/null", O_WRONLY | O_CLOEXEC);
    if (devnull >= 0) {
        if (!log_enabled(Log::Info))
            dup2(devnull, STDOUT_FILENO);
        if (!log_enabled(Log::Error))
            dup2(devnull, STDERR_FILENO);
    }
    const char* path = getenv("ATRIUM_XWAYLAND");
    execvp(path ? path : ATRIUM_XWAYLAND_PATH, const_cast<char* const*>(argv));
    alog_errno(Log::Error, "xwayland: can't run %s", path ? path : ATRIUM_XWAYLAND_PATH);
    _exit(EXIT_FAILURE);
}

int Server::on_ready(int fd, uint32_t mask, void* data) {
    auto* s = static_cast<Server*>(data);
    if (mask & WL_EVENT_READABLE) {
        // It writes the display number and a newline, maybe in two writes:
        // closing early would fail the second and stop it.
        char buf[64];
        const ssize_t n = read(fd, buf, sizeof buf);
        if (n < 0 && errno != EINTR) {
            alog_errno(Log::Error, "xwayland: reading its display fd failed");
            mask = 0;
        } else if (n <= 0 || buf[n - 1] != '\n') {
            return 1;  // more to come
        }
    }
    // The first fork's exit (another SIGCHLD handler may have reaped it).
    while (waitpid(s->pid_, nullptr, 0) < 0 && errno == EINTR) {
    }
    close(fd);
    wl_event_source_remove(s->pipe_source_);
    s->pipe_source_ = nullptr;
    if (!(mask & WL_EVENT_READABLE)) {
        alog(Log::Error, "xwayland: Xwayland didn't start");
        s->finish_process();
        s->finish_display();
        return 0;
    }
    alog(Log::Debug, "xwayland: Xwayland is ready");
    s->events.ready.emit(s->wm_fd_[0]);
    return 0;
}

void Server::finish_process() {
    if (client_) {
        wl_list_remove(&client_destroy_.link);
        wl_client* c = client_;
        client_ = nullptr;
        wl_client_destroy(c);
    }
    if (pipe_source_) {
        wl_event_source_remove(pipe_source_);
        pipe_source_ = nullptr;
    }
    safe_close(wl_fd_[0]);
    safe_close(wl_fd_[1]);
    safe_close(wm_fd_[0]);
    safe_close(wm_fd_[1]);
    pid_ = 0;
    // Xwayland isn't killed: it goes when its sockets to us close, and the
    // pid may not be Xwayland's any more.
}

void Server::finish_display() {
    if (display_ < 0)
        return;
    safe_close(x_fd_[0]);
    safe_close(x_fd_[1]);
    if (!held_)
        unlink_display_sockets(display_);
    display_ = -1;
    display_name_.clear();
}

} // namespace atrium::xwayland
#endif
