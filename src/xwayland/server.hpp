#pragma once
// Xwayland itself: an X display's sockets and lock file, the process on
// them as a Wayland client of ours, and the window manager's socket. A port
// of wlroots' xwayland/server.c and sockets.c (MIT).
#include "wl/signal.hpp"

#include <wayland-server-core.h>

#include <ctime>
#include <string>
#include <sys/types.h>

namespace atrium::xwayland {

// The first free display number's sockets (abstract and in /tmp/.X11-unix)
// and lock file: the number, -1 if none of :0 to :32 is free.
int open_display_sockets(int socks[2]);
void unlink_display_sockets(int display);

class Server {
public:
    // Picks a display and starts Xwayland on it once the loop idles; ok()
    // says whether there was a display (and an Xwayland to run).
    explicit Server(wl_display* display);
    ~Server();
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    bool ok() const { return display_ >= 0; }
    const char* display_name() const { return display_name_.c_str(); }
    // Xwayland as our client (null between runs).
    wl_client* client() const { return client_; }

    struct {
        wl::Signal<> start;     // client() is made; Xwayland is being started
        wl::Signal<int> ready;  // it's up: the WM end of its -wm socket
    } events;

private:
    bool start();
    void finish_process();
    void finish_display();
    [[noreturn]] void exec(int notify_fd);
    static int on_ready(int fd, uint32_t mask, void* data);

    wl_display* wl_display_;
    // This run's.
    pid_t pid_ = 0;
    wl_client* client_ = nullptr;
    wl_event_source* pipe_source_ = nullptr;
    int wm_fd_[2] = {-1, -1}, wl_fd_[2] = {-1, -1};
    time_t started_ = 0;
    wl_listener client_destroy_{};
    // Kept across restarts.
    int display_ = -1;
    bool held_ = false;  // the crash-recovery wrapper's display: its to unlink
    std::string display_name_;
    int x_fd_[2] = {-1, -1};
    wl_event_source* idle_ = nullptr;
};

} // namespace atrium::xwayland
