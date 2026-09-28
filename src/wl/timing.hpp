#pragma once
#include "wl/compositor.hpp"

#include <ctime>
#include <map>

namespace atrium::wl {

class Output;

// wp_presentation: when a surface's content reached the screen.
class Presentation {
public:
    explicit Presentation(wl_display* display);
    ~Presentation();

    enum Flags : uint32_t { Vsync = 1, HwClock = 2, HwCompletion = 4, ZeroCopy = 8 };
    // `surface`'s current content was shown on `output` at `when`.
    static void presented(Surface* surface, Output* output, const timespec& when, uint32_t refresh_ns, uint64_t seq,
                          uint32_t flags);

private:
    std::unique_ptr<Global> global_;
    std::vector<Weak<Resource>> managers_;
};

// wp_fifo_manager_v1: an app's commits wait for the screen (a video player
// that doesn't want to skip frames without blocking in eglSwapBuffers).
class Fifo {
public:
    explicit Fifo(wl_display* display);
    ~Fifo();

    // The output `surface` is on refreshed (or it isn't shown at all): its
    // barrier lifts and the next waiting commit goes in.
    void refreshed(Surface* surface);

private:
    struct State;
    std::unique_ptr<Global> global_;
    std::vector<Weak<Resource>> managers_;
    std::map<Surface*, std::unique_ptr<State>> surfaces_;
};

// wp_commit_timing_manager_v1: a commit that shouldn't apply before a time.
class CommitTiming {
public:
    explicit CommitTiming(wl_display* display);
    ~CommitTiming();

private:
    struct Waiting;
    wl_display* display_;
    std::unique_ptr<Global> global_;
    std::vector<Weak<Resource>> managers_;
    std::map<Surface*, Resource*> timers_;
    std::vector<std::unique_ptr<Waiting>> waiting_;
};

// wp_linux_drm_syncobj_manager_v1: explicit sync, which NVIDIA needs. A
// commit waits (off the GPU) until its acquire point exists; its release
// point is signalled once the compositor is done reading the buffer.
class Syncobj {
public:
    // `drm_fd`: the render node timelines are imported on.
    Syncobj(wl_display* display, int drm_fd);
    ~Syncobj();

    // The compositor read `surface`'s current buffer in work that finishes
    // at `point` on `timeline`: its release waits for that too.
    void add_release_point(Surface* surface, wlr_drm_syncobj_timeline* timeline, uint64_t point);

private:
    struct Waiter;
    struct Release;
    struct PerSurface;
    int drm_fd_;
    wl_display* display_;
    std::unique_ptr<Global> global_;
    std::vector<Weak<Resource>> managers_;
    std::map<Surface*, std::unique_ptr<PerSurface>> surfaces_;
    std::vector<std::unique_ptr<Waiter>> waiters_;
    std::vector<Release*> releases_;
    friend struct Release;
};

} // namespace atrium::wl
