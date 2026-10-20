#pragma once
// A screen's frames as dmabufs (ext-image-copy-capture into GBM buffers):
// the compositor blits each new frame into one of a few buffers on its own
// GPU, and the newest stays there to be sampled whenever the recorder's
// clock wants one (gpu-screen-recorder grabs the scanout plane by KMS for the
// same; atrium is the DRM master, so the copy comes over Wayland instead).

#include "ext-image-capture-source-v1-client-protocol.h"
#include "ext-image-copy-capture-v1-client-protocol.h"
#include "linux-dmabuf-v1-client-protocol.h"
#include "xdg-output-unstable-v1-client-protocol.h"

#include <sys/types.h>
#include <wayland-client.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct gbm_bo;
struct gbm_device;

namespace atrium::record {

struct Dmabuf {
    int fd[4] = {-1, -1, -1, -1};
    uint32_t offset[4] = {}, stride[4] = {};
    int planes = 0;
    uint32_t fourcc = 0;
    uint64_t modifier = 0;
    int width = 0, height = 0;
    uint64_t id = 0;  // stays the same for the buffer's life (what the GPU side caches its import by)
};

struct Screen {
    wl_output* wl = nullptr;
    zxdg_output_v1* xdg = nullptr;
    std::string name;
    int width = 0, height = 0, refresh_mhz = 0;  // its mode
    int32_t scale = 1, transform = 0;
    int x = 0, y = 0, logical_width = 0, logical_height = 0;  // in the desktop
};

class Capture {
public:
    ~Capture();
    // Wayland and the globals; false (with a message) when something's missing.
    bool connect(std::string& error);
    std::vector<std::unique_ptr<Screen>>& screens() { return screens_; }

    // Copies of `screen` from now on, 10-bit buffers when `ten_bit` (an HDR
    // screen's frames would lose their depth in 8). Waits for the
    // compositor's constraints and allocates the buffers. `interval_ns`, the
    // recording's frame time, paces the copies.
    bool start(Screen& screen, bool cursor, bool ten_bit, int64_t interval_ns, std::string& error);

    int fd() const;
    // Reads and handles what's come; false when the connection broke.
    bool dispatch();
    bool flush();

    // Asks for the next frame when one isn't on its way and pacing allows.
    void pump(int64_t now_ns);
    // When `pump` next wants to run (ns, monotonic); INT64_MAX: not before a frame comes.
    int64_t next_wake() const { return next_request_ns_; }

    // The newest complete frame, its count (grows with each new one) and its
    // buffer transform; null before the first.
    const Dmabuf* latest() const { return latest_ >= 0 ? &pool_[latest_].dmabuf : nullptr; }
    uint64_t frames() const { return frames_; }
    int32_t frame_transform() const { return frame_transform_; }
    // The compositor stopped the session (the screen went away).
    bool stopped() const { return stopped_; }

    // The compositor's GPU, as it said (the one its frames live on).
    dev_t device() const { return device_; }
    const std::string& render_node() const { return render_node_; }
    int width() const { return width_; }
    int height() const { return height_; }

private:
    struct Buffer {
        gbm_bo* bo = nullptr;
        wl_buffer* wl = nullptr;
        Dmabuf dmabuf;
    };
    struct Format {
        uint32_t fourcc;
        std::vector<uint64_t> modifiers;
    };

    static const wl_registry_listener kRegistry;
    static const ext_image_copy_capture_session_v1_listener kSession;
    static const ext_image_copy_capture_frame_v1_listener kFrame;

    bool allocate(std::string& error);
    void free_pool();
    void request();

    wl_display* display_ = nullptr;
    wl_registry* registry_ = nullptr;
    ext_image_copy_capture_manager_v1* copy_ = nullptr;
    ext_output_image_capture_source_manager_v1* sources_ = nullptr;
    zwp_linux_dmabuf_v1* dmabuf_ = nullptr;
    zxdg_output_manager_v1* xdg_ = nullptr;
    std::vector<std::unique_ptr<Screen>> screens_;

    ext_image_copy_capture_session_v1* session_ = nullptr;
    ext_image_copy_capture_frame_v1* frame_ = nullptr;
    int pending_ = -1, latest_ = -1;
    uint64_t frames_ = 0;
    int32_t frame_transform_ = 0, pending_transform_ = 0;
    bool stopped_ = false, constraints_done_ = false, reallocate_ = false;
    bool ten_bit_ = false;
    int64_t next_request_ns_ = 0, refresh_ns_ = 0, interval_ns_ = 0;

    // The session's constraints.
    int width_ = 0, height_ = 0;
    dev_t device_ = 0;
    std::vector<Format> formats_;
    std::string render_node_;
    gbm_device* gbm_ = nullptr;
    int gbm_fd_ = -1;
    std::vector<Buffer> pool_;
    uint64_t next_id_ = 1;
};

} // namespace atrium::record
