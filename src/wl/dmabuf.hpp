#pragma once
#include "wl/buffer.hpp"
#include "wl/compositor.hpp"

#include <sys/types.h>

#include <functional>
#include <map>
#include <optional>
#include <string>

namespace atrium::wl {

// What a client may allocate: per device, the formats and modifiers the
// compositor can use, best first (a scanout tranche for a fullscreen window
// before the render one).
struct DmabufFeedback {
    struct Tranche {
        dev_t target_device = 0;
        bool scanout = false;
        std::vector<std::pair<uint32_t, uint64_t>> formats;  // DRM fourcc, modifier
    };
    dev_t main_device = 0;
    std::vector<Tranche> tranches;
};

// zwp_linux_dmabuf_v1 (v5, with feedback): buffers in GPU memory. `check`
// says whether the compositor can use a buffer (imported on the main device,
// say); a buffer it refuses fails to be made.
class LinuxDmabuf {
public:
    using Check = std::function<bool(const wlr_dmabuf_attributes&)>;

    LinuxDmabuf(wl_display* display, DmabufFeedback feedback, Check check);
    ~LinuxDmabuf();
    LinuxDmabuf(const LinuxDmabuf&) = delete;
    LinuxDmabuf& operator=(const LinuxDmabuf&) = delete;

    // What `surface` is told instead of the default (a scanout tranche while
    // it could go straight to the screen); nullopt goes back to the default.
    void set_surface_feedback(Surface* surface, std::optional<DmabufFeedback> feedback);

    // The attributes behind a dmabuf wl_buffer's wlr_buffer, if it is one.
    static bool is_dmabuf(wlr_buffer* buffer);

private:
    struct Table;
    struct SurfaceFeedback;
    std::shared_ptr<Table> table_for(const DmabufFeedback& feedback);
    void send(Resource* feedback_resource, const DmabufFeedback& feedback, const Table& table);

    DmabufFeedback default_;
    std::shared_ptr<Table> default_table_;
    Check check_;
    std::unique_ptr<Global> global_;
    std::vector<Weak<Resource>> managers_, default_feedbacks_;
    std::map<Surface*, std::unique_ptr<SurfaceFeedback>> surfaces_;
};

// wl_drm, the legacy Mesa protocol: the render node's name and PRIME
// buffers, for old EGL clients.
class LegacyDrm {
public:
    LegacyDrm(wl_display* display, std::string node, std::vector<uint32_t> formats, LinuxDmabuf::Check check);
    ~LegacyDrm();

private:
    std::string node_;
    std::vector<uint32_t> formats_;
    LinuxDmabuf::Check check_;
    std::unique_ptr<Global> global_;
    std::vector<Weak<Resource>> resources_;
};

} // namespace atrium::wl
