#pragma once
#include "wl/desktop.hpp"
#include "wl/seat.hpp"

#include <sys/types.h>

#include <ctime>
#include <functional>
#include <optional>

namespace atrium::wl {

// Screen and window capture, for every protocol that asks for it
// (wlr-screencopy, ext-image-copy-capture, hyprland-toplevel-export,
// wlr-export-dmabuf). The protocols differ in shape, not in what they ask:
// the compositor answers the same few questions for all of them.
class Capture {
public:
    // What is captured.
    struct Target {
        Output* output = nullptr;  // a screen...
        ForeignToplevels::Handle* toplevel = nullptr;  // ...or a window
        std::optional<Box> region;  // part of the screen, logical coordinates
        bool cursor = false;  // with the pointer drawn in
        bool cursor_only = false;  // just the pointer's image (ext cursor sessions)
        bool operator==(const Target&) const = default;
    };
    // The buffers a capture of a target fits in.
    struct Constraints {
        int width = 0, height = 0;
        uint32_t shm_format = 0;  // DRM fourcc
        uint32_t shm_stride = 0;
        std::optional<uint32_t> dmabuf_format;
        std::vector<uint64_t> dmabuf_modifiers;
        dev_t dmabuf_device = 0;
        bool operator==(const Constraints&) const = default;
    };
    struct Result {
        bool ok = false;
        timespec when{};
        std::vector<Box> damage;  // buffer coordinates; empty: all
        uint32_t transform = 0;
        bool y_invert = false;
        uint32_t fail_reason = 0;  // ext: 0 unknown, 1 buffer constraints, 2 stopped
    };
    // A copy to make: render `target` into `buffer`, then call done once.
    struct Copy {
        Target target;
        wlr_buffer* buffer;
        bool wait_for_damage;  // only when something changed (screencopy with damage)
        std::function<void(const Result&)> done;
    };
    // wlr-export-dmabuf: the screen's own frame, shared as it is.
    struct Export {
        Output* output;
        bool cursor;
        std::function<void(const wlr_dmabuf_attributes* attrs, const timespec& when)> done;  // null: cancelled
    };

    Capture(wl_display* display, Seat& seat, ForeignToplevels& toplevels);
    ~Capture();

    // Asked for each target a client wants to capture; nullopt: can't.
    std::function<std::optional<Constraints>(const Target&)> constraints;
    Signal<Copy&> copy;
    Signal<Export&> export_frame;

    // A target's size or formats changed (a screen changed mode, a window
    // resized): ext sessions hear the new constraints.
    void constraints_changed(const Target& target);
    // A target is gone (screen unplugged, window closed): its sessions stop.
    void stop(const Target& target);
    // Where the pointer is over a target, for ext cursor sessions (null: not
    // over it).
    void cursor(const Target& target, std::optional<std::pair<int, int>> position, std::pair<int, int> hotspot);

private:
    struct Session;
    struct CursorSession;
    void send_constraints(Session* s);

    wl_display* display_;
    Seat& seat_;
    ForeignToplevels& toplevels_;
    std::unique_ptr<Global> screencopy_, export_dmabuf_, output_sources_, toplevel_sources_, image_copy_, hypr_export_;
    std::vector<Weak<Resource>> managers_, frames_, sources_;
    std::vector<std::unique_ptr<Session>> sessions_;
    std::vector<std::unique_ptr<CursorSession>> cursor_sessions_;
};

} // namespace atrium::wl
