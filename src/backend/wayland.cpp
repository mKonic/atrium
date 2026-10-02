#include "backend/wayland.hpp"
#include "listener.hpp"

#include "common.hpp"

#include <wayland-client.h>

#include "linux-dmabuf-v1-client-protocol.h"
#include "presentation-time-client-protocol.h"
#include "xdg-decoration-unstable-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"

#include <drm_fourcc.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <xf86drm.h>

#include <algorithm>
#include <cstring>
#include <string>

namespace atrium::backend {

namespace {
size_t g_last_window = 0;
constexpr int kDefaultWidth = 1280, kDefaultHeight = 720;
} // namespace

// One of ours, as the host knows it.
struct Wayland::RemoteBuffer {
    Wayland* backend = nullptr;
    Buffer* buffer = nullptr;
    struct wl_buffer* remote = nullptr;
    bool busy = false;  // the host holds it (we hold a lock on ours)
    Listener<> destroy;

    ~RemoteBuffer() {
        if (remote)
            wl_buffer_destroy(remote);
    }
};

// ---- a screen: a window on the host -------------------------------------------------

class Wayland::Window final : public Output {
public:
    Window(Wayland& b) : Output(b), owner(b) {
        const size_t n = ++g_last_window;
        name = "WL-" + std::to_string(n);
        description = "atrium window " + std::to_string(n);
        make = "atrium";
        model = "nested";
        width = kDefaultWidth;
        height = kDefaultHeight;
        refresh = 0;
    }

    ~Window() override {
        if (frame_cb_)
            wl_callback_destroy(frame_cb_);
        for (Feedback* f : feedbacks_) {
            wp_presentation_feedback_destroy(f->feedback);
            delete f;
        }
        if (cursor_surface_)
            wl_surface_destroy(cursor_surface_);
        if (decoration_)
            zxdg_toplevel_decoration_v1_destroy(decoration_);
        if (toplevel_)
            xdg_toplevel_destroy(toplevel_);
        if (xdg_surface_)
            xdg_surface_destroy(xdg_surface_);
        if (surface_)
            wl_surface_destroy(surface_);
        if (cursor_buffer_)
            buffer_unlock(cursor_buffer_);
    }

    // The window, mapped once the host has configured it.
    bool create() {
        surface_ = wl_compositor_create_surface(owner.compositor_);
        if (!surface_)
            return false;
        wl_surface_set_user_data(surface_, this);
        xdg_surface_ = xdg_wm_base_get_xdg_surface(owner.wm_base_, surface_);
        xdg_surface_add_listener(xdg_surface_, &kXdgSurface, this);
        toplevel_ = xdg_surface_get_toplevel(xdg_surface_);
        xdg_toplevel_add_listener(toplevel_, &kToplevel, this);
        xdg_toplevel_set_app_id(toplevel_, "atrium");
        xdg_toplevel_set_title(toplevel_, ("atrium - " + name).c_str());
        if (owner.decorations_) {
            decoration_ = zxdg_decoration_manager_v1_get_toplevel_decoration(owner.decorations_, toplevel_);
            zxdg_toplevel_decoration_v1_set_mode(decoration_, ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
        }
        wl_surface_commit(surface_);
        // The first configure, before anything is drawn.
        for (int i = 0; i < 10 && !configured_; ++i)
            if (wl_display_roundtrip(owner.remote_) < 0)
                return false;
        return configured_;
    }

    bool has_cursor_plane() const override { return true; }
    const FormatSet* cursor_formats(uint32_t) const override { return &owner.formats_; }
    const FormatSet* primary_formats(uint32_t) const override { return &owner.formats_; }
    bool direct_scanout_allowed() const override { return false; }

    bool set_cursor(Buffer* buffer, int hx, int hy) override {
        if (cursor_buffer_)
            buffer_unlock(cursor_buffer_);
        cursor_buffer_ = buffer ? buffer_lock(buffer) : nullptr;
        cursor_hx_ = hx;
        cursor_hy_ = hy;
        if (!cursor_surface_)
            cursor_surface_ = wl_compositor_create_surface(owner.compositor_);
        if (buffer) {
            struct wl_buffer* rb = owner.remote_buffer(buffer);
            if (!rb)
                return false;
            wl_surface_attach(cursor_surface_, rb, 0, 0);
            wl_surface_damage_buffer(cursor_surface_, 0, 0, INT32_MAX, INT32_MAX);
        } else {
            wl_surface_attach(cursor_surface_, nullptr, 0, 0);
        }
        wl_surface_commit(cursor_surface_);
        show_cursor();
        return true;
    }
    bool move_cursor(int, int) override { return true; }  // the host moves it

    // The host's pointer came over this window: our image on it.
    void show_cursor() {
        if (!owner.pointer_ || owner.pointer_focus_ != this)
            return;
        wl_pointer_set_cursor(owner.pointer_, owner.enter_serial_, cursor_buffer_ ? cursor_surface_ : nullptr,
                              cursor_hx_, cursor_hy_);
    }

    wl_surface* surface() const { return surface_; }
    Wayland& owner;

protected:
    bool test(const OutputState& s) override {
        constexpr uint32_t kSupported = OutputState::Buffer | OutputState::Damage | OutputState::ModeField |
                                        OutputState::Enabled | OutputState::Scale | OutputState::Transform |
                                        OutputState::RenderFormat | OutputState::Subpixel;
        if (s.committed & ~kSupported)
            return false;
        if ((s.committed & OutputState::ModeField) && s.mode_type != OutputState::ModeType::Custom)
            return false;
        if (s.committed & OutputState::Buffer) {
            DmabufAttributes a;
            if (!buffer_get_dmabuf(s.buffer, &a) || !owner.formats_.has(a.format, a.modifier))
                return false;
        }
        return true;
    }

    bool commit(const OutputState& s) override {
        if (!test(s))
            return false;
        if ((s.committed & OutputState::Enabled) && !s.enabled) {
            wl_surface_attach(surface_, nullptr, 0, 0);
            wl_surface_commit(surface_);
            wl_display_flush(owner.remote_);
            return true;
        }
        if (s.committed & OutputState::Buffer) {
            struct wl_buffer* rb = owner.remote_buffer(s.buffer);
            if (!rb)
                return false;
            RemoteBuffer* r = owner.buffers_.at(s.buffer).get();
            if (!r->busy) {
                r->busy = true;
                buffer_lock(s.buffer);
            }
            wl_surface_attach(surface_, rb, 0, 0);
            if (s.committed & OutputState::Damage) {
                int n = 0;
                const pixman_box32_t* rects = pixman_region32_rectangles(&s.damage, &n);
                for (int i = 0; i < n; ++i)
                    wl_surface_damage_buffer(surface_, rects[i].x1, rects[i].y1, rects[i].x2 - rects[i].x1,
                                             rects[i].y2 - rects[i].y1);
            } else {
                wl_surface_damage_buffer(surface_, 0, 0, INT32_MAX, INT32_MAX);
            }
            if (!frame_cb_) {
                frame_cb_ = wl_surface_frame(surface_);
                wl_callback_add_listener(frame_cb_, &kFrame, this);
            }
            if (owner.presentation_) {
                auto* f = new Feedback{this, wp_presentation_feedback(owner.presentation_, surface_), commit_seq + 1};
                wp_presentation_feedback_add_listener(f->feedback, &kFeedback, f);
                feedbacks_.push_back(f);
            }
        }
        wl_surface_commit(surface_);
        wl_display_flush(owner.remote_);
        if ((s.committed & OutputState::Buffer) && !owner.presentation_) {
            Present p;
            p.commit_seq = commit_seq + 1;
            p.presented = true;
            send_present(p);
        }
        return true;
    }

private:
    struct Feedback {
        Window* window;
        struct wp_presentation_feedback* feedback;
        uint32_t seq;
    };

    void forget(Feedback* f) {
        std::erase(feedbacks_, f);
        wp_presentation_feedback_destroy(f->feedback);
        delete f;
    }

    static const xdg_surface_listener kXdgSurface;
    static const xdg_toplevel_listener kToplevel;
    static const wl_callback_listener kFrame;
    static const wp_presentation_feedback_listener kFeedback;

    wl_surface* surface_ = nullptr;
    xdg_surface* xdg_surface_ = nullptr;
    xdg_toplevel* toplevel_ = nullptr;
    zxdg_toplevel_decoration_v1* decoration_ = nullptr;
    wl_callback* frame_cb_ = nullptr;
    std::vector<Feedback*> feedbacks_;
    bool configured_ = false;
    int32_t pending_w_ = 0, pending_h_ = 0;

    wl_surface* cursor_surface_ = nullptr;
    Buffer* cursor_buffer_ = nullptr;
    int cursor_hx_ = 0, cursor_hy_ = 0;
};

const xdg_surface_listener Wayland::Window::kXdgSurface = {
    .configure =
        [](void* data, xdg_surface* xs, uint32_t serial) {
            auto* w = static_cast<Window*>(data);
            xdg_surface_ack_configure(xs, serial);
            w->configured_ = true;
            // The host resized the window: the screen follows.
            if (w->pending_w_ > 0 && w->pending_h_ > 0 && (w->pending_w_ != w->width || w->pending_h_ != w->height)) {
                OutputState s;
                s.set_custom_mode(w->pending_w_, w->pending_h_, 0);
                w->events.request_state.emit(s);
            }
        },
};

const xdg_toplevel_listener Wayland::Window::kToplevel = {
    .configure =
        [](void* data, xdg_toplevel*, int32_t width, int32_t height, wl_array*) {
            auto* w = static_cast<Window*>(data);
            w->pending_w_ = width;
            w->pending_h_ = height;
        },
    .close = [](void* data, xdg_toplevel*) {
        auto* w = static_cast<Window*>(data);
        w->owner.destroy_output(w);
    },
    .configure_bounds = [](void*, xdg_toplevel*, int32_t, int32_t) {},
    .wm_capabilities = [](void*, xdg_toplevel*, wl_array*) {},
};

const wl_callback_listener Wayland::Window::kFrame = {
    .done =
        [](void* data, wl_callback* cb, uint32_t) {
            auto* w = static_cast<Window*>(data);
            wl_callback_destroy(cb);
            w->frame_cb_ = nullptr;
            w->send_frame();
        },
};

const wp_presentation_feedback_listener Wayland::Window::kFeedback = {
    .sync_output = [](void*, struct wp_presentation_feedback*, wl_output*) {},
    .presented =
        [](void* data, struct wp_presentation_feedback*, uint32_t sec_hi, uint32_t sec_lo, uint32_t nsec, uint32_t refresh,
           uint32_t seq_hi, uint32_t seq_lo, uint32_t flags) {
            auto* f = static_cast<Feedback*>(data);
            Window* w = f->window;
            Present p;
            p.commit_seq = f->seq;
            p.presented = true;
            p.when.tv_sec = time_t((uint64_t(sec_hi) << 32) | sec_lo);
            p.when.tv_nsec = long(nsec);
            p.refresh = int(refresh);
            p.seq = unsigned((uint64_t(seq_hi) << 32) | seq_lo);
            p.flags = flags;
            if (w->owner.presentation_clock_ != CLOCK_MONOTONIC)
                p.when = {};  // a clock we can't compare: "now"
            w->forget(f);
            w->send_present(p);
        },
    .discarded =
        [](void* data, struct wp_presentation_feedback*) {
            auto* f = static_cast<Feedback*>(data);
            Window* w = f->window;
            Present p;
            p.commit_seq = f->seq;
            p.presented = false;
            w->forget(f);
            w->send_present(p);
        },
};

// ---- the host connection ---------------------------------------------------------------

namespace {

// The render node of the host's main device.
int open_main_device(const wl_array* devs) {
    if (devs->size < sizeof(dev_t))
        return -1;
    dev_t dev;
    std::memcpy(&dev, devs->data, sizeof(dev));
    drmDevice* d = nullptr;
    if (drmGetDeviceFromDevId(dev, 0, &d) != 0)
        return -1;
    int fd = -1;
    const int node = (d->available_nodes & (1 << DRM_NODE_RENDER)) ? DRM_NODE_RENDER : DRM_NODE_PRIMARY;
    if (d->available_nodes & (1 << node))
        fd = open(d->nodes[node], O_RDWR | O_CLOEXEC);
    drmFreeDevice(&d);
    return fd;
}

struct FormatTableEntry {
    uint32_t format;
    uint32_t pad;
    uint64_t modifier;
};

struct FeedbackState {
    Wayland* backend;
    int* drm_fd;
    FormatSet* formats;
    const FormatTableEntry* table = nullptr;
    size_t table_len = 0;
    bool done = false;
};

const zwp_linux_dmabuf_feedback_v1_listener kDmabufFeedback = {
    .done = [](void* data, zwp_linux_dmabuf_feedback_v1*) { static_cast<FeedbackState*>(data)->done = true; },
    .format_table =
        [](void* data, zwp_linux_dmabuf_feedback_v1*, int32_t fd, uint32_t size) {
            auto* st = static_cast<FeedbackState*>(data);
            void* map = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
            close(fd);
            if (map == MAP_FAILED)
                return;
            st->table = static_cast<const FormatTableEntry*>(map);
            st->table_len = size / sizeof(FormatTableEntry);
        },
    .main_device =
        [](void* data, zwp_linux_dmabuf_feedback_v1*, wl_array* dev) {
            auto* st = static_cast<FeedbackState*>(data);
            if (*st->drm_fd < 0)
                *st->drm_fd = open_main_device(dev);
        },
    .tranche_done = [](void*, zwp_linux_dmabuf_feedback_v1*) {},
    .tranche_target_device = [](void*, zwp_linux_dmabuf_feedback_v1*, wl_array*) {},
    .tranche_formats =
        [](void* data, zwp_linux_dmabuf_feedback_v1*, wl_array* indices) {
            auto* st = static_cast<FeedbackState*>(data);
            const auto* idx = static_cast<const uint16_t*>(indices->data);
            for (size_t i = 0; st->table && i < indices->size / sizeof(uint16_t); ++i)
                if (idx[i] < st->table_len)
                    st->formats->add(st->table[idx[i]].format, st->table[idx[i]].modifier);
        },
    .tranche_flags = [](void*, zwp_linux_dmabuf_feedback_v1*, uint32_t) {},
};

const zwp_linux_dmabuf_v1_listener kDmabuf = {
    .format = [](void*, zwp_linux_dmabuf_v1*, uint32_t) {},
    .modifier =
        [](void* data, zwp_linux_dmabuf_v1*, uint32_t format, uint32_t hi, uint32_t lo) {
            static_cast<FormatSet*>(data)->add(format, (uint64_t(hi) << 32) | lo);
        },
};

const xdg_wm_base_listener kWmBase = {
    .ping = [](void*, xdg_wm_base* base, uint32_t serial) { xdg_wm_base_pong(base, serial); },
};

const wp_presentation_listener kPresentation = {
    .clock_id = [](void* data, wp_presentation*, uint32_t clk) { *static_cast<clockid_t*>(data) = clockid_t(clk); },
};

} // namespace

Wayland::Wayland(wl_event_loop* loop) : Backend(loop) {}

std::unique_ptr<Wayland> Wayland::create(wl_event_loop* loop) {
    std::unique_ptr<Wayland> b(new Wayland(loop));
    if (!b->connect())
        return nullptr;
    return b;
}

// The host's seat: its pointer and keyboard over our windows.
const wl_pointer_listener Wayland::kPointer = {
    .enter =
        [](void* data, wl_pointer*, uint32_t serial, wl_surface* surface, wl_fixed_t x, wl_fixed_t y) {
            auto* b = static_cast<Wayland*>(data);
            b->enter_serial_ = serial;
            b->pointer_focus_ = b->window_of(surface);
            if (Window* w = b->pointer_focus_) {
                w->show_cursor();
                b->events.host_motion.emit(w, 0, wl_fixed_to_double(x) / std::max(w->width, 1),
                                           wl_fixed_to_double(y) / std::max(w->height, 1));
            }
        },
    .leave = [](void* data, wl_pointer*, uint32_t, wl_surface*) {
        static_cast<Wayland*>(data)->pointer_focus_ = nullptr;
    },
    .motion =
        [](void* data, wl_pointer*, uint32_t time, wl_fixed_t x, wl_fixed_t y) {
            auto* b = static_cast<Wayland*>(data);
            if (Window* w = b->pointer_focus_)
                b->events.host_motion.emit(w, time ? time : 1, wl_fixed_to_double(x) / std::max(w->width, 1),
                                           wl_fixed_to_double(y) / std::max(w->height, 1));
        },
    .button =
        [](void* data, wl_pointer*, uint32_t, uint32_t time, uint32_t button, uint32_t state) {
            static_cast<Wayland*>(data)->events.host_button.emit(time, button,
                                                                state == WL_POINTER_BUTTON_STATE_PRESSED);
        },
    .axis =
        [](void* data, wl_pointer*, uint32_t time, uint32_t axis, wl_fixed_t value) {
            auto* b = static_cast<Wayland*>(data);
            if (axis > 1)
                return;
            b->events.host_axis.emit(
                {time, axis, wl_fixed_to_double(value), b->value120_[axis], b->axis_source_, false});
            b->value120_[axis] = 0;
        },
    .frame =
        [](void* data, wl_pointer*) {
            auto* b = static_cast<Wayland*>(data);
            b->events.host_frame.emit();
            b->axis_source_ = 0;
        },
    .axis_source = [](void* data, wl_pointer*, uint32_t source) { static_cast<Wayland*>(data)->axis_source_ = source; },
    .axis_stop = [](void*, wl_pointer*, uint32_t, uint32_t) {},
    .axis_discrete = [](void*, wl_pointer*, uint32_t, int32_t) {},
    .axis_value120 =
        [](void* data, wl_pointer*, uint32_t axis, int32_t v) {
            if (axis <= 1)
                static_cast<Wayland*>(data)->value120_[axis] += v;
        },
    .axis_relative_direction = [](void*, wl_pointer*, uint32_t, uint32_t) {},
};
const wl_keyboard_listener Wayland::kKeyboard = {
    .keymap = [](void*, wl_keyboard*, uint32_t, int32_t fd, uint32_t) { close(fd); },  // ours is atrium's own
    .enter =
        [](void* data, wl_keyboard*, uint32_t, wl_surface*, wl_array* keys) {
            auto* b = static_cast<Wayland*>(data);
            timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            const auto t = uint32_t(now.tv_sec * 1000 + now.tv_nsec / 1000000);
            const auto* k = static_cast<const uint32_t*>(keys->data);
            for (size_t i = 0; i < keys->size / sizeof(uint32_t); ++i) {
                b->keys_down_.push_back(k[i]);
                b->events.host_key.emit(t, k[i], true);
            }
        },
    .leave =
        [](void* data, wl_keyboard*, uint32_t, wl_surface*) {
            // Whatever was held is let go: the host keeps its keys now.
            auto* b = static_cast<Wayland*>(data);
            timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            const auto t = uint32_t(now.tv_sec * 1000 + now.tv_nsec / 1000000);
            for (uint32_t k : std::exchange(b->keys_down_, {}))
                b->events.host_key.emit(t, k, false);
        },
    .key =
        [](void* data, wl_keyboard*, uint32_t, uint32_t time, uint32_t key, uint32_t state) {
            auto* b = static_cast<Wayland*>(data);
            const bool pressed = state == WL_KEYBOARD_KEY_STATE_PRESSED;
            if (pressed)
                b->keys_down_.push_back(key);
            else
                std::erase(b->keys_down_, key);
            b->events.host_key.emit(time, key, pressed);
        },
    .modifiers = [](void*, wl_keyboard*, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) {},
    .repeat_info = [](void*, wl_keyboard*, int32_t, int32_t) {},
};
const wl_seat_listener Wayland::kSeat = {
    .capabilities =
        [](void* data, wl_seat* seat, uint32_t caps) {
            auto* b = static_cast<Wayland*>(data);
            if ((caps & WL_SEAT_CAPABILITY_POINTER) && !b->pointer_) {
                b->pointer_ = wl_seat_get_pointer(seat);
                wl_pointer_add_listener(b->pointer_, &kPointer, b);
            } else if (!(caps & WL_SEAT_CAPABILITY_POINTER) && b->pointer_) {
                wl_pointer_release(b->pointer_);
                b->pointer_ = nullptr;
                b->pointer_focus_ = nullptr;
            }
            if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !b->keyboard_) {
                b->keyboard_ = wl_seat_get_keyboard(seat);
                wl_keyboard_add_listener(b->keyboard_, &kKeyboard, b);
            } else if (!(caps & WL_SEAT_CAPABILITY_KEYBOARD) && b->keyboard_) {
                wl_keyboard_release(b->keyboard_);
                b->keyboard_ = nullptr;
            }
        },
    .name = [](void*, wl_seat*, const char*) {},
};

bool Wayland::connect() {
    remote_ = wl_display_connect(nullptr);
    if (!remote_) {
        alog(Log::Error, "nested: no Wayland compositor to connect to");
        return false;
    }
    static const wl_registry_listener kRegistry = {
        .global =
            [](void* data, wl_registry* reg, uint32_t name, const char* iface, uint32_t version) {
                auto* b = static_cast<Wayland*>(data);
                const std::string_view i = iface;
                if (i == wl_compositor_interface.name) {
                    b->compositor_ = static_cast<wl_compositor*>(
                        wl_registry_bind(reg, name, &wl_compositor_interface, std::min(version, 4u)));
                } else if (i == xdg_wm_base_interface.name) {
                    b->wm_base_ = static_cast<xdg_wm_base*>(wl_registry_bind(reg, name, &xdg_wm_base_interface, 1));
                    xdg_wm_base_add_listener(b->wm_base_, &kWmBase, b);
                } else if (i == zxdg_decoration_manager_v1_interface.name) {
                    b->decorations_ = static_cast<zxdg_decoration_manager_v1*>(
                        wl_registry_bind(reg, name, &zxdg_decoration_manager_v1_interface, 1));
                } else if (i == zwp_linux_dmabuf_v1_interface.name && version >= 3) {
                    b->dmabuf_ = static_cast<zwp_linux_dmabuf_v1*>(
                        wl_registry_bind(reg, name, &zwp_linux_dmabuf_v1_interface, std::min(version, 4u)));
                    if (version < 4)
                        zwp_linux_dmabuf_v1_add_listener(b->dmabuf_, &kDmabuf, &b->formats_);
                } else if (i == wp_presentation_interface.name) {
                    b->presentation_ =
                        static_cast<wp_presentation*>(wl_registry_bind(reg, name, &wp_presentation_interface, 1));
                    wp_presentation_add_listener(b->presentation_, &kPresentation, &b->presentation_clock_);
                } else if (i == wl_seat_interface.name && !b->seat_) {
                    b->seat_ = static_cast<wl_seat*>(wl_registry_bind(reg, name, &wl_seat_interface, std::min(version, 8u)));
                    // Now: its capabilities come with the first roundtrip.
                    wl_seat_add_listener(b->seat_, &kSeat, b);
                }
            },
        .global_remove = [](void*, wl_registry*, uint32_t) {},
    };
    registry_ = wl_display_get_registry(remote_);
    wl_registry_add_listener(registry_, &kRegistry, this);
    if (wl_display_roundtrip(remote_) < 0)
        return false;
    if (!compositor_ || !wm_base_ || !dmabuf_) {
        alog(Log::Error, "nested: the host lacks wl_compositor, xdg_wm_base or linux-dmabuf");
        return false;
    }
    // What buffers it takes, and on which GPU.
    if (zwp_linux_dmabuf_v1_get_version(dmabuf_) >= 4) {
        FeedbackState st{this, &drm_fd_, &formats_};
        zwp_linux_dmabuf_feedback_v1* fb = zwp_linux_dmabuf_v1_get_default_feedback(dmabuf_);
        zwp_linux_dmabuf_feedback_v1_add_listener(fb, &kDmabufFeedback, &st);
        while (!st.done)
            if (wl_display_roundtrip(remote_) < 0)
                break;
        zwp_linux_dmabuf_feedback_v1_destroy(fb);
        if (st.table)
            munmap(const_cast<FormatTableEntry*>(st.table), st.table_len * sizeof(FormatTableEntry));
    } else {
        wl_display_roundtrip(remote_);
    }
    if (formats_.empty()) {
        alog(Log::Error, "nested: the host takes no dmabuf formats");
        return false;
    }

    // The seat's capabilities: its pointer and keyboard.
    wl_display_roundtrip(remote_);

    // Its events, read as they come; what we send, flushed every loop turn.
    source_ = wl_event_loop_add_fd(
        loop(), wl_display_get_fd(remote_), WL_EVENT_READABLE,
        [](int, uint32_t mask, void* data) {
            auto* b = static_cast<Wayland*>(data);
            if (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) {
                b->hangup();
                return 0;
            }
            int n = 0;
            if (mask & WL_EVENT_READABLE)
                n = wl_display_dispatch(b->remote_);
            else
                n = wl_display_dispatch_pending(b->remote_);
            if (n < 0) {
                b->hangup();
                return 0;
            }
            wl_display_flush(b->remote_);
            return n;
        },
        this);
    wl_event_source_check(source_);
    return true;
}

void Wayland::hangup() {
    if (gone_)
        return;
    gone_ = true;
    alog(Log::Error, "nested: the host compositor went away");
    if (source_) {
        wl_event_source_remove(source_);
        source_ = nullptr;
    }
    events.gone.emit();
}

Wayland::~Wayland() {
    for (Window* w : std::vector(windows_))
        destroy_output(w);
    // Ours again, whatever the host still held.
    std::vector<Buffer*> held;
    for (auto& [buf, r] : buffers_)
        if (std::exchange(r->busy, false))
            held.push_back(buf);
    buffers_.clear();
    for (Buffer* b : held)
        buffer_unlock(b);
    if (source_)
        wl_event_source_remove(source_);
    if (pointer_)
        wl_pointer_release(pointer_);
    if (keyboard_)
        wl_keyboard_release(keyboard_);
    if (seat_)
        wl_seat_destroy(seat_);
    if (presentation_)
        wp_presentation_destroy(presentation_);
    if (dmabuf_)
        zwp_linux_dmabuf_v1_destroy(dmabuf_);
    if (decorations_)
        zxdg_decoration_manager_v1_destroy(decorations_);
    if (wm_base_)
        xdg_wm_base_destroy(wm_base_);
    if (compositor_)
        wl_compositor_destroy(compositor_);
    if (registry_)
        wl_registry_destroy(registry_);
    if (remote_) {
        wl_display_flush(remote_);
        wl_display_disconnect(remote_);
    }
    if (drm_fd_ >= 0)
        close(drm_fd_);
    events.destroy.emit();
}

uint32_t Wayland::buffer_caps() const {
    return BUFFER_CAP_DMABUF;
}

bool Wayland::start() {
    started_ = true;
    // As many windows as WLR_WL_OUTPUTS asks for (one by default).
    const char* n = getenv("WLR_WL_OUTPUTS");
    const long count = n && *n ? std::strtol(n, nullptr, 10) : 1;
    for (long i = 0; i < count; ++i)
        if (!create_output())
            return false;
    return true;
}

Output* Wayland::create_output() {
    if (gone_)
        return nullptr;
    auto* w = new Window(*this);
    if (!w->create()) {
        delete w;
        return nullptr;
    }
    windows_.push_back(w);
    if (started_)
        events.new_output.emit(w);
    return w;
}

bool Wayland::is_virtual(const Output* o) const {
    return std::ranges::find(windows_, o) != windows_.end();
}

bool Wayland::destroy_output(Output* o) {
    auto it = std::ranges::find(windows_, o);
    if (it == windows_.end())
        return false;
    Window* w = *it;
    windows_.erase(it);
    if (pointer_focus_ == w)
        pointer_focus_ = nullptr;
    w->events.destroy.emit();
    delete w;
    if (remote_)
        wl_display_flush(remote_);
    return true;
}

Wayland::Window* Wayland::window_of(const void* surface) const {
    for (Window* w : windows_)
        if (w->surface() == surface)
            return w;
    return nullptr;
}

struct wl_buffer* Wayland::remote_buffer(Buffer* buffer) {
    if (auto it = buffers_.find(buffer); it != buffers_.end())
        return it->second->remote;
    DmabufAttributes a;
    if (!buffer_get_dmabuf(buffer, &a))
        return nullptr;
    zwp_linux_buffer_params_v1* params = zwp_linux_dmabuf_v1_create_params(dmabuf_);
    for (int i = 0; i < a.n_planes; ++i)
        zwp_linux_buffer_params_v1_add(params, a.fd[i], uint32_t(i), a.offset[i], a.stride[i],
                                       uint32_t(a.modifier >> 32), uint32_t(a.modifier & 0xffffffff));
    struct wl_buffer* remote = zwp_linux_buffer_params_v1_create_immed(params, a.width, a.height, a.format, 0);
    zwp_linux_buffer_params_v1_destroy(params);
    if (!remote)
        return nullptr;

    auto r = std::make_unique<RemoteBuffer>();
    r->backend = this;
    r->buffer = buffer;
    r->remote = remote;
    static const wl_buffer_listener kRelease = {
        .release =
            [](void* data, struct wl_buffer*) {
                auto* r = static_cast<RemoteBuffer*>(data);
                if (!r->busy)
                    return;
                r->busy = false;
                buffer_unlock(r->buffer);  // may destroy it, and r with it
            },
    };
    wl_buffer_add_listener(remote, &kRelease, r.get());
    RemoteBuffer* raw = r.get();
    raw->destroy.connect(&buffer->events.destroy, [this, buffer](void*) { buffers_.erase(buffer); });
    buffers_[buffer] = std::move(r);
    return remote;
}

} // namespace atrium::backend
