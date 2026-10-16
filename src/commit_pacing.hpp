#pragma once
// fifo-v1 and commit-timing-v1: apps (Mesa's Vulkan FIFO present mode,
// games pacing their frames) hold a commit back until the screen has shown
// the one before (a barrier), or until a time. Specced from Hyprland
// (protocols/Fifo.cpp, CommitTiming.cpp) and KWin: a held commit is a lock
// on the surface's cached state (wlr_surface_lock_pending), let go on the
// next refresh of a screen the surface is on. Never held while it's on no
// screen or its screen tears, and never for long: the protocol asks that a
// wait ends in finite time.

#include <wayland-server-core.h>

#include <ctime>
#include <deque>
#include <memory>
#include <unordered_map>

struct wlr_surface;
struct wlr_output;

namespace atrium {

class Server;

class CommitPacing {
public:
    explicit CommitPacing(Server& server);
    ~CommitPacing();
    CommitPacing(const CommitPacing&) = delete;
    CommitPacing& operator=(const CommitPacing&) = delete;

    struct Surface;
    struct Screen;

    // A refresh cycle of `output` went by: barriers there clear, and commits
    // whose time has come go through.
    void refreshed(wlr_output* output);
    Surface* surface_for(wlr_surface* s, bool create);
    void drop(Surface* s);
    void watch(wlr_output* output);
    bool tearing(wlr_output* output) const;

    Server& server;

private:
    wl_global* fifo_global_ = nullptr;
    wl_global* timing_global_ = nullptr;
    std::unordered_map<wlr_surface*, std::unique_ptr<Surface>> surfaces_;
    std::unordered_map<wlr_output*, std::unique_ptr<Screen>> screens_;
    wl_event_source* safety_ = nullptr;
};

} // namespace atrium
