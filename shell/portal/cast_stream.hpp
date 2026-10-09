#pragma once
// One shared screen or window: an ext-image-copy-capture session on
// atrium-portal's Wayland connection feeding a PipeWire video source, as
// xdg-desktop-portal-wlr does it (ext_image_copy.c, pipewire_screencast.c).
// PipeWire picks dma-bufs (with a modifier both ends take) or shared memory;
// each frame is copied into the buffer PipeWire hands out, at most at the
// screen's refresh rate.

#include "cast_core.hpp"

#include <QObject>
#include <QString>
#include <QTimer>

#include <spa/param/video/raw.h>
#include <spa/pod/builder.h>
#include <spa/utils/hook.h>

#include <chrono>
#include <list>
#include <memory>
#include <vector>

struct ext_image_capture_source_v1;
struct ext_image_copy_capture_session_v1;
struct ext_image_copy_capture_frame_v1;
struct gbm_device;
struct pw_buffer;
struct pw_stream;
struct wl_buffer;

namespace atrium {

class CastStream : public QObject {
    Q_OBJECT

public:
    struct Target {
        uint32_t type = cast::Monitor;
        QString output;    // a screen's connector name
        QString toplevel;  // a window's ext-foreign-toplevel identifier
        bool cursor = true;
    };

    CastStream(const Target& target, QObject* parent);
    ~CastStream() override;

    // Starts capturing; false when it can't (the source is gone, no capture
    // protocol). ready() follows once PipeWire has numbered the node.
    bool start();
    // Waits (running PipeWire's loop) until ready(), at most `ms`.
    bool wait_ready(int ms);
    const Target& target() const { return target_; }
    uint32_t node() const { return node_; }
    uint64_t serial() const { return serial_; }
    int width() const { return current_.width; }
    int height() const { return current_.height; }

    // Listener plumbing (the C callbacks reach these).
    struct Constraints {
        int width = 0, height = 0;
        std::vector<std::pair<uint32_t, int>> shm;  // fourcc, stride
        dev_t device = 0;
        std::vector<std::pair<uint32_t, uint64_t>> dmabuf;  // fourcc, modifier
        bool operator==(const Constraints&) const = default;
    };
    Constraints pending_;
    void constraints_done();
    void session_stopped();
    void frame_damage(const cast::Rect& r);
    void frame_time(uint64_t sec, uint32_t nsec);
    void frame_transform(uint32_t t);
    void frame_ready();
    void frame_failed(uint32_t reason);
    void stream_state(int old_state, int state);
    void stream_param(uint32_t id, const spa_pod* param);
    void stream_add_buffer(pw_buffer* b);
    void stream_remove_buffer(pw_buffer* b);
    void stream_process();

signals:
    void ready();
    void stopped();

private:
    struct Buffer {
        wl_buffer* wl = nullptr;
        bool dmabuf = false;
        int planes = 0;
        int fd[4] = {-1, -1, -1, -1};
        uint32_t size[4] = {}, stride[4] = {}, offset[4] = {};
        std::vector<cast::Rect> damage;  // what this buffer misses since it was last filled
    };

    void update_gbm();
    void update_params();
    // All the formats on offer, dma-buf ones first.
    std::vector<const spa_pod*> formats(spa_pod_builder* b);
    std::unique_ptr<Buffer> make_buffer();
    bool dequeue();
    void enqueue();
    void capture();       // the next frame, as soon as the frame rate allows
    void capture_now();
    void stop();

    Target target_;
    ext_image_capture_source_v1* source_ = nullptr;
    ext_image_copy_capture_session_v1* session_ = nullptr;
    ext_image_copy_capture_frame_v1* frame_ = nullptr;
    Constraints current_;
    gbm_device* gbm_ = nullptr;
    pw_stream* stream_ = nullptr;
    spa_hook stream_listener_{};
    spa_video_info_raw format_{};
    bool use_dmabuf_ = false, avoid_dmabuf_ = false;
    bool streaming_ = false, announced_ = false, stopping_ = false;
    uint32_t node_ = 0xffffffff;
    uint64_t serial_ = 0;
    uint32_t framerate_ = 0;  // the most frames a second (0: no limit)
    uint64_t seq_ = 0;
    std::list<std::unique_ptr<Buffer>> buffers_;
    // The frame being taken.
    pw_buffer* current_pw_ = nullptr;
    Buffer* buffer_ = nullptr;
    bool completed_ = false;
    std::vector<cast::Rect> damage_;
    uint64_t pts_ns_ = 0;
    uint32_t transform_ = 0;
    std::chrono::steady_clock::time_point last_start_{};
    QTimer pace_, retry_;
};

} // namespace atrium
