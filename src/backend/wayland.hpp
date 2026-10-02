#pragma once
// atrium nested in another compositor: each screen is a window there, its
// frames handed over as dmabufs, and the host's pointer and keyboard over
// those windows are atrium's input. After wlroots' Wayland backend (MIT).
#include "render/fwd.hpp"
#include "util/buffer.hpp"
#include "backend/backend.hpp"

#include <memory>
#include <unordered_map>
#include <vector>

struct wl_display;
struct wl_registry;
struct wl_compositor;
struct wl_seat;
struct wl_pointer;
struct wl_keyboard;
struct xdg_wm_base;
struct zxdg_decoration_manager_v1;
struct zwp_linux_dmabuf_v1;
struct wp_presentation;
struct wl_seat_listener;
struct wl_pointer_listener;
struct wl_keyboard_listener;

namespace atrium::backend {

class Wayland final : public Backend {
public:
    // Connected to the host named by WAYLAND_DISPLAY; null if there is none.
    static std::unique_ptr<Wayland> create(wl_event_loop* loop);
    ~Wayland() override;

    bool start() override;
    int drm_fd() const override { return drm_fd_; }
    uint32_t buffer_caps() const override;
    Output* create_output() override;
    bool is_virtual(const Output* o) const override;
    bool destroy_output(Output* o) override;

private:
    class Window;
    struct RemoteBuffer;
    friend class Window;

    explicit Wayland(wl_event_loop* loop);
    bool connect();
    Window* window_of(const void* surface) const;
    // The host's wl_buffer for one of ours (made once, kept with it).
    struct wl_buffer* remote_buffer(Buffer* buffer);
    void hangup();
    static const ::wl_seat_listener kSeat;
    static const ::wl_pointer_listener kPointer;
    static const ::wl_keyboard_listener kKeyboard;

    struct wl_display* remote_ = nullptr;
    wl_registry* registry_ = nullptr;
    wl_compositor* compositor_ = nullptr;
    xdg_wm_base* wm_base_ = nullptr;
    zxdg_decoration_manager_v1* decorations_ = nullptr;
    zwp_linux_dmabuf_v1* dmabuf_ = nullptr;
    wp_presentation* presentation_ = nullptr;
    clockid_t presentation_clock_ = CLOCK_MONOTONIC;
    wl_seat* seat_ = nullptr;
    wl_pointer* pointer_ = nullptr;
    wl_keyboard* keyboard_ = nullptr;
    wl_event_source* source_ = nullptr;
    int drm_fd_ = -1;
    wlr_drm_format_set formats_{};  // what the host takes as dmabufs
    bool started_ = false;
    bool gone_ = false;

    std::vector<Window*> windows_;
    std::unordered_map<Buffer*, std::unique_ptr<RemoteBuffer>> buffers_;

    // The host's seat.
    Window* pointer_focus_ = nullptr;
    uint32_t enter_serial_ = 0;
    uint32_t axis_source_ = 0;
    int32_t value120_[2] = {0, 0};
    std::vector<uint32_t> keys_down_;
};

} // namespace atrium::backend
