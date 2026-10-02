#include "seat.hpp"
#include "keyboard_conf.hpp"
#include "input_method.hpp"

#include "devices.hpp"
#include "geometry.hpp"
#include "layer_surface.hpp"
#include "ipc.hpp"
#include "output.hpp"
#include "overview.hpp"
#include "switcher.hpp"
#include "server.hpp"
#include "space.hpp"
#include "snap_preview.hpp"
#include "toplevel_drag.hpp"
#include "titlebar.hpp"
#include "view.hpp"
#ifdef ATRIUM_XWAYLAND
#include "xwayland/xwm.hpp"
#endif

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <ctime>

namespace atrium {

namespace {

xkb_keymap* compile_keymap(const Config& c) {
    auto opt = [](const std::string& s) { return s.empty() ? nullptr : s.c_str(); };
    // No layout set: the system's keyboard, as a whole.
    const XkbNames& sys = system_keyboard();
    const bool own = !c.xkb_layout.empty();
    xkb_rule_names names{};
    names.rules = opt(c.xkb_rules);
    names.model = opt(c.xkb_model.empty() ? sys.model : c.xkb_model);
    names.layout = opt(own ? c.xkb_layout : sys.layout);
    names.variant = opt(own ? c.xkb_variant : sys.variant);
    std::string options = c.xkb_options.empty() ? sys.options : c.xkb_options;
    for (const std::string& own : {c.xkb_switch, c.xkb_compose})
        if (!own.empty())
            options += (options.empty() ? "" : ",") + own;
    names.options = opt(options);

    xkb_context* ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    xkb_keymap* keymap = xkb_keymap_new_from_names(ctx, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
    if (!keymap) {
        wlr_log(WLR_ERROR, "keymap '%s' failed to compile, falling back to the default",
                c.xkb_layout.c_str());
        keymap = xkb_keymap_new_from_names(ctx, nullptr, XKB_KEYMAP_COMPILE_NO_FLAGS);
    }
    xkb_context_unref(ctx);
    return keymap;
}

uint32_t now_ms() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return uint32_t(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

} // namespace

// --- keyboard groups -------------------------------------------------------------

KeyboardGroup::KeyboardGroup(Seat& s, bool virt) : seat(s), is_virtual(virt) {
    xkb_keymap* keymap = compile_keymap(seat.server.config);
    keys.set_keymap(keymap);
    xkb_keymap_unref(keymap);
    keys.set_repeat(seat.server.config.repeat_rate, seat.server.config.repeat_delay);
    keys.on_key = [this](uint32_t time, uint32_t keycode, bool pressed) { seat.key(*this, time, keycode, pressed); };
    keys.on_modifiers = [this] { seat.modifiers(*this); };
    repeat_source = wl_event_loop_add_timer(seat.server.loop, [](void* data) {
        auto* g = static_cast<KeyboardGroup*>(data);
        return g->seat.key_repeat(*g);
    }, this);
}

KeyboardGroup::~KeyboardGroup() {
    wl_event_source_remove(repeat_source);
    connections.clear();
    if (seat.seat_kb_ == this)
        seat.seat_kb_ = nullptr;
}

// --- seat ----------------------------------------------------------------------

namespace {

AxisEvent axis_of(const wlr_pointer_axis_event* e) {
    return {e->time_msec, uint32_t(e->orientation), e->delta, e->delta_discrete, uint32_t(e->source),
            e->relative_direction == WL_POINTER_AXIS_RELATIVE_DIRECTION_INVERTED};
}

} // namespace

Seat::Seat(Server& srv) : server(srv) {
    wl::Seat& ws = *server.wl->seat;
    cursor = wlr_cursor_create();
    wlr_cursor_attach_output_layout(cursor, server.output_layout);
    apply_cursor_theme();

    cursor_motion_.connect(&cursor->events.motion, [this](wlr_pointer_motion_event* e) {
        motion(e->time_msec, &e->pointer->base, e->delta_x, e->delta_y, e->unaccel_dx, e->unaccel_dy);
    });
    cursor_motion_absolute_.connect(&cursor->events.motion_absolute,
        [this](wlr_pointer_motion_absolute_event* e) {
            // Virtual pointers send time 0 and expect a warp.
            if (!e->time_msec)
                wlr_cursor_warp_absolute(cursor, &e->pointer->base, e->x, e->y);
            double lx, ly;
            wlr_cursor_absolute_to_layout_coords(cursor, &e->pointer->base, e->x, e->y, &lx, &ly);
            double dx = lx - cursor->x, dy = ly - cursor->y;
            motion(e->time_msec, &e->pointer->base, dx, dy, dx, dy);
        });
    // A nested backend's pointers (the host's) come through the cursor.
    cursor_button_.connect(&cursor->events.button, [this](wlr_pointer_button_event* e) {
        button(ButtonEvent{e->time_msec, e->button, e->state == WL_POINTER_BUTTON_STATE_PRESSED});
    });
    cursor_axis_.connect(&cursor->events.axis, [this](wlr_pointer_axis_event* e) { axis(axis_of(e)); });
    cursor_frame_.connect(&cursor->events.frame, [this](void*) { server.wl->seat->pointer_frame(); });

    auto& c = connections_;
    c.push_back(ws.events.request_cursor.connect([this](const wl::Seat::CursorRequest& r) {
        // While we own the pointer (move/resize) the client's image waits.
        if ((mode != Mode::Normal && mode != Mode::Pressed) || shaking_)
            return;
        wl::Surface* focus = server.wl->seat->pointer_focus();
        if (focus && focus->client() == r.client)
            set_cursor_surface(r.surface, r.hotspot_x, r.hotspot_y);
    }));
    c.push_back(server.wl->cursor_shapes->request_shape.connect([this](const wl::CursorShapes::Request& r) {
        if ((mode != Mode::Normal && mode != Mode::Pressed) || shaking_ || r.tablet_tool)
            return;
        wl::Surface* focus = server.wl->seat->pointer_focus();
        if (focus && focus->client() == r.client) {
            set_cursor_surface(nullptr, 0, 0);
            wlr_cursor_set_xcursor(cursor, xcursor, wl::CursorShapes::name_of(r.shape));
        }
    }));
    // A drag needs a press it can quote; a selection is granted (the
    // protocol layer checks the client may set it).
    c.push_back(server.wl->data->events.request_drag.connect([this](const wl::DataDevices::DragRequest& r) {
        if (r.origin && server.wl->seat->validate_grab_serial(r.origin->client(), r.serial))
            server.wl->data->start_drag(r.source, r.origin, r.icon);
        else if (r.source)
            r.source->cancelled();
    }));
    c.push_back(server.wl->data->events.drag_started.connect([this](wl::Drag* d) { start_drag(d); }));
    c.push_back(server.wl->pointer_constraints->events.destroy.connect([this](wl::PointerConstraints::Constraint* k) {
        if (active_constraint_ == k) {
            warp_to_constraint_hint();
            active_constraint_ = nullptr;
        }
    }));

    new_input_.connect(&server.backend->events.new_input, [this](wlr_input_device* d) { new_input(d); });
    c.push_back(server.wl->virtual_inputs->new_keyboard.connect(
        [this](wl::VirtualInputs::Keyboard* vk) { new_virtual_keyboard(vk); }));
    c.push_back(server.wl->virtual_inputs->new_pointer.connect(
        [this](wl::VirtualInputs::Pointer* vp) { new_virtual_pointer(vp); }));

    keyboards_ = std::make_unique<KeyboardGroup>(*this, false);
    use_keyboard(physical_keyboard(), true);
    // On a real session, input straight from libinput.
    libinput_ = input::Libinput::create(server.session, server.loop, *this);
    update_capabilities();
}

void Seat::session_active(bool active) {
    if (libinput_)
        libinput_->set_active(active);
}

void Seat::new_virtual_keyboard(wl::VirtualInputs::Keyboard* vk) {
    // Its own group: synthetic input never shares modifier state with the
    // physical keyboard.
    auto group = std::make_unique<KeyboardGroup>(*this, true);
    KeyboardGroup* g = group.get();
    g->owner = vk->resource ? vk->resource->client() : nullptr;
    auto set_keymap = [g, vk] {
        xkb_context* ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
        if (xkb_keymap* km = xkb_keymap_new_from_string(ctx, vk->keymap.c_str(), XKB_KEYMAP_FORMAT_TEXT_V1,
                                                        XKB_KEYMAP_COMPILE_NO_FLAGS)) {
            g->keys.set_keymap(km);
            xkb_keymap_unref(km);
        }
        xkb_context_unref(ctx);
        // Its keymap goes to clients when it next speaks.
        if (g->seat.seat_kb_ == g)
            g->seat.use_keyboard(g, true);
    };
    if (!vk->keymap.empty())
        set_keymap();
    g->connections.push_back(vk->keymap_changed.connect(set_keymap));
    // It sends its modifiers itself: keys don't change them.
    g->connections.push_back(vk->key.connect(
        [g](uint32_t time, uint32_t key, bool pressed) { g->keys.key(time, key, pressed, false); }));
    g->connections.push_back(vk->modifiers.connect([g](const wl::Seat::Modifiers& m) {
        g->keys.set_modifiers({m.depressed, m.latched, m.locked, m.group});
    }));
    g->connections.push_back(vk->destroy.connect([this, g] {
        std::erase_if(virtual_keyboards_, [g](auto& p) { return p.get() == g; });
        // Gone (the IME quit): the seat's keyboard is the real one again.
        if (!seat_kb_)
            use_keyboard(physical_keyboard());
        update_capabilities();
    }));
    virtual_keyboards_.push_back(std::move(group));
    update_capabilities();
}

void Seat::new_virtual_pointer(wl::VirtualInputs::Pointer* vp) {
    // Its screen: absolute motion spans it (else every screen).
    auto layout = [this, vp](double x, double y, double* lx, double* ly) {
        const Output* o = vp->output ? static_cast<Output*>(vp->output->data) : nullptr;
        const wlr_box b = o ? o->box : server.layout_box;
        *lx = b.x + x * b.width;
        *ly = b.y + y * b.height;
    };
    auto source = std::make_shared<uint32_t>(WL_POINTER_AXIS_SOURCE_WHEEL);
    std::vector<wl::Connection> c;
    c.push_back(vp->motion.connect([this](uint32_t time, double dx, double dy) { motion(time, nullptr, dx, dy, dx, dy); }));
    c.push_back(vp->motion_absolute.connect([this, layout](uint32_t time, double x, double y) {
        double lx, ly;
        layout(x, y, &lx, &ly);
        motion_absolute(time, lx, ly);
    }));
    c.push_back(vp->button.connect(
        [this](uint32_t time, uint32_t b, bool pressed) { button(ButtonEvent{time, b, pressed}); }));
    c.push_back(vp->axis_source.connect([source](uint32_t s) { *source = s; }));
    c.push_back(vp->axis.connect([this, source](uint32_t time, uint32_t a, double value, int32_t discrete) {
        axis(AxisEvent{time, a, value, discrete * 120, *source, false});
    }));
    c.push_back(vp->axis_stop.connect(
        [this, source](uint32_t time, uint32_t a) { axis(AxisEvent{time, a, 0, 0, *source, false}); }));
    c.push_back(vp->frame.connect([this] { server.wl->seat->pointer_frame(); }));
    auto* list = &virtual_pointers_.emplace_back();
    c.push_back(vp->destroy.connect([this, list] {
        // Off its signals after this one returns: it is calling us.
        wl_event_loop_add_idle(server.loop, [](void* data) {
            auto* seat = static_cast<Seat*>(data);
            std::erase_if(seat->virtual_pointers_, [](const auto& v) {
                return std::ranges::none_of(v, [](const wl::Connection& k) { return k.connected(); });
            });
        }, this);
        for (auto& k : *list)
            k.disconnect();
    }));
    *list = std::move(c);
}

// The keyboard clients hear: its keymap goes out when it changes.
void Seat::use_keyboard(KeyboardGroup* kb, bool force) {
    if (!kb || (kb == seat_kb_ && !force))
        return;
    seat_kb_ = kb;
    if (xkb_keymap* keymap = kb->keys.keymap()) {
        char* text = xkb_keymap_get_as_string(keymap, XKB_KEYMAP_FORMAT_TEXT_V1);
        if (text && sent_keymap_ != text) {
            sent_keymap_ = text;
            server.wl->seat->set_keymap(sent_keymap_);
            // Clients start this keymap with no modifiers: its own go along.
            const input::Modifiers& m = kb->keys.modifiers();
            server.wl->seat->keyboard_modifiers({m.depressed, m.latched, m.locked, m.group});
        }
        free(text);
    }
    server.wl->seat->set_repeat_info(kb->keys.repeat_rate, kb->keys.repeat_delay);
}

void Seat::set_cursor_surface(wl::Surface* surface, int hot_x, int hot_y) {
    cursor_commit_.disconnect();
    cursor_gone_.disconnect();
    cursor_surface_ = surface;
    cursor_hot_x_ = hot_x;
    cursor_hot_y_ = hot_y;
    if (!surface) {
        wlr_cursor_unset_image(cursor);
        return;
    }
    auto show = [this] {
        wl::Surface* s = cursor_surface_;
        // Moved by its attach offset: the hotspot moves against it.
        cursor_hot_x_ -= s->current().dx;
        cursor_hot_y_ -= s->current().dy;
        if (wlr_buffer* b = s->buffer())
            wlr_cursor_set_buffer(cursor, b, cursor_hot_x_ * s->current().scale, cursor_hot_y_ * s->current().scale,
                                  float(s->current().scale));
        else
            wlr_cursor_unset_image(cursor);
        timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        s->send_frame_done(uint32_t(int64_t(now.tv_sec) * 1000 + now.tv_nsec / 1000000));
    };
    cursor_commit_ = surface->events.commit.connect(show);
    cursor_gone_ = surface->events.destroy.connect([this] {
        cursor_commit_.disconnect();
        cursor_gone_.disconnect();
        cursor_surface_ = nullptr;
        wlr_cursor_unset_image(cursor);
    });
    if (wlr_buffer* b = surface->buffer())
        wlr_cursor_set_buffer(cursor, b, hot_x * surface->current().scale, hot_y * surface->current().scale,
                              float(surface->current().scale));
    else
        wlr_cursor_unset_image(cursor);
}

void Seat::start_drag(wl::Drag* drag) {
    if (wl::Surface* icon = drag->icon()) {
        drag_icon_ = scene::drag_icon_create(server.drag_icons, icon);
        drag_icon_gone_.connect(&drag_icon_->events.destroy, [this](void*) {
            drag_icon_ = nullptr;
            drag_icon_gone_.disconnect();
        });
    }
    server.wl->seat->pointer_clear_focus();
    drag_ended_ = drag->ended.connect([this] {
        drag_ended_.disconnect();
        if (drag_icon_) {
            drag_icon_gone_.disconnect();
            drag_icon_->destroy();
            drag_icon_ = nullptr;
        }
        // Focus enter is not sent during a drag; restore it now.
        wl_event_loop_add_idle(server.loop, [](void* data) {
            auto* seat = static_cast<Seat*>(data);
            seat->server.focus_top();
            seat->refresh_pointer();
        }, this);
    });
    refresh_pointer();
}

Seat::~Seat() {
    // Off every signal before the objects carrying them go away.
    new_input_.disconnect();
    cursor_motion_.disconnect();
    cursor_motion_absolute_.disconnect();
    cursor_button_.disconnect();
    cursor_axis_.disconnect();
    cursor_frame_.disconnect();
    connections_.clear();
    cursor_commit_.disconnect();
    cursor_gone_.disconnect();
    drag_ended_.disconnect();
    drag_icon_gone_.disconnect();
    virtual_pointers_.clear();
    libinput_.reset();
    pointers_.clear();
    virtual_keyboards_.clear();
    keyboards_.reset();
    server.animator.cancel_owner(&shake_, false);
    if (shake_end_)
        wl_event_source_remove(shake_end_);
    for (wlr_xcursor_manager* m : shake_xcursor_)
        if (m)
            wlr_xcursor_manager_destroy(m);
    wlr_xcursor_manager_destroy(xcursor);
    wlr_cursor_destroy(cursor);
}

#ifdef ATRIUM_XWAYLAND
void Seat::set_x11_cursor() {
    if (!server.xwm)
        return;
    wlr_xcursor_manager_load(xcursor, 1);
    if (wlr_xcursor* xc = wlr_xcursor_manager_get_xcursor(xcursor, "default", 1)) {
        const wlr_xcursor_image* img = xc->images[0];
        server.xwm->set_cursor(img->buffer, img->width * 4, int(img->width), int(img->height), int(img->hotspot_x),
                               int(img->hotspot_y));
    }
}
#endif

void Seat::apply_keyboard_config() {
    xkb_keymap* keymap = compile_keymap(server.config);
    for (KeyboardGroup* g : {keyboards_.get()}) {
        g->keys.set_keymap(keymap);
        g->keys.set_repeat(server.config.repeat_rate, server.config.repeat_delay);
    }
    // Virtual keyboards keep the keymap their client gave; repeat is ours.
    for (auto& g : virtual_keyboards_)
        g->keys.set_repeat(server.config.repeat_rate, server.config.repeat_delay);
    xkb_keymap_unref(keymap);
    use_keyboard(physical_keyboard(), true);
    // A new keymap starts at its first layout, and the names may differ.
    last_layout_ = layout();
    server.keyboard_layout_changed();
}

void Seat::apply_cursor_theme() {
    if (xcursor)
        wlr_xcursor_manager_destroy(xcursor);
    const Config& c = server.config;
    const char* theme = c.cursor_theme.empty() ? getenv("XCURSOR_THEME") : c.cursor_theme.c_str();
    xcursor = wlr_xcursor_manager_create(theme, c.cursor_size);
    for (int i = 0; i < kShakeLevels; ++i) {
        if (shake_xcursor_[i])
            wlr_xcursor_manager_destroy(shake_xcursor_[i]);
        shake_xcursor_[i] = wlr_xcursor_manager_create(theme, uint32_t(std::lround(c.cursor_size * (1.5 + 0.5 * i))));
    }
    setenv("XCURSOR_SIZE", std::to_string(c.cursor_size).c_str(), 1);
    if (theme)
        setenv("XCURSOR_THEME", theme, 1);
}

// --- shake to find ---------------------------------------------------------

void Seat::show_shake_level(int level) {
    shake_level_ = level;
    wlr_cursor_set_xcursor(cursor, level ? shake_xcursor_[level - 1] : xcursor, "default");
}

// The arrow grows while the shaking goes on, and settles once it stops.
void Seat::shake_grow() {
    constexpr int kHoldMs = 600;  // still big this long after the last shake
    if (!shake_end_)
        shake_end_ = wl_event_loop_add_timer(server.loop, [](void* data) {
            static_cast<Seat*>(data)->shake_settle();
            return 0;
        }, this);
    wl_event_source_timer_update(shake_end_, kHoldMs);
    if (shaking_)
        return;
    shaking_ = true;
    server.animator.cancel_owner(&shake_, false);
    const int from = shake_level_;
    server.animator.start(&shake_, 150, Ease::EmphasizedDecel, [this, from](double t) {
        show_shake_level(from + int(std::lround((kShakeLevels - from) * t)));
    });
}

void Seat::shake_settle() {
    shake_.reset();
    server.animator.cancel_owner(&shake_, false);
    const int from = shake_level_;
    server.animator.start(&shake_, 300, Ease::Standard, [this, from](double t) {
        show_shake_level(int(std::lround(from * (1 - t))));
    }, [this] {
        // The app under the pointer sets its own cursor again, as on entering.
        shaking_ = false;
        if (mode == Mode::Normal) {
            server.wl->seat->pointer_clear_focus();
            refresh_pointer();
        }
    });
}

void Seat::set_default_cursor() {
    wlr_cursor_set_xcursor(cursor, xcursor, "default");
}

// --- devices -----------------------------------------------------------------

// A nested backend's devices: the host's keyboard and pointer.
void Seat::new_input(wlr_input_device* device) {
    switch (device->type) {
    case WLR_INPUT_DEVICE_KEYBOARD:
        add_keyboard(wlr_keyboard_from_input_device(device));
        break;
    case WLR_INPUT_DEVICE_POINTER:
        add_pointer(wlr_pointer_from_input_device(device));
        break;
    default:
        break;
    }
    update_capabilities();
}

void Seat::add_keyboard(wlr_keyboard* keyboard) {
    auto kb = std::make_unique<PhysicalKeyboard>();
    PhysicalKeyboard* raw = kb.get();
    raw->wlr = keyboard;
    // Its keys go through atrium's own state; the host's modifiers (and the
    // host's layout in them) are not ours.
    raw->key.connect(&keyboard->events.key, [this](wlr_keyboard_key_event* e) {
        keyboards_->keys.key(e->time_msec, e->keycode, e->state == WL_KEYBOARD_KEY_STATE_PRESSED);
    });
    raw->destroy.connect(&keyboard->base.events.destroy, [this, raw](void*) {
        std::erase_if(physical_, [raw](auto& k) { return k.get() == raw; });
        update_capabilities();
    });
    physical_.push_back(std::move(kb));
}

uint32_t Seat::layout() const {
    return keyboards_->keys.layout();
}

std::vector<std::string> Seat::layout_names() const {
    std::vector<std::string> names;
    if (xkb_keymap* keymap = keyboards_->keys.keymap())
        for (xkb_layout_index_t i = 0; i < xkb_keymap_num_layouts(keymap); ++i) {
            const char* name = xkb_keymap_layout_get_name(keymap, i);
            names.emplace_back(name ? name : "");
        }
    return names;
}

void Seat::set_layout(uint32_t index) {
    xkb_keymap* keymap = keyboards_->keys.keymap();
    const uint32_t count = keymap ? xkb_keymap_num_layouts(keymap) : 0;
    if (count == 0)
        return;
    index %= count;
    const bool changed = index != last_layout_;
    last_layout_ = index;
    keyboards_->keys.set_layout(index);
    if (changed)
        server.keyboard_layout_changed();
}

void Seat::add_pointer(wlr_pointer* pointer) {
    wlr_cursor_attach_input_device(cursor, &pointer->base);
    auto dev = std::make_unique<PointerDevice>(pointer);
    PointerDevice* raw = dev.get();
    raw->destroy.connect(&pointer->base.events.destroy, [this, raw](void*) {
        std::erase_if(pointers_, [raw](auto& p) { return p.get() == raw; });
    });
    pointers_.push_back(std::move(dev));
}

std::vector<std::pair<std::string, libinput_device*>> Seat::pointer_devices() const {
    std::vector<std::pair<std::string, libinput_device*>> out;
    if (libinput_)
        for (const auto& d : libinput_->devices())
            if (d->pointer)
                out.emplace_back(d->name, d->handle);
    for (const auto& p : pointers_)
        out.emplace_back(p->wlr->base.name ? p->wlr->base.name : "", nullptr);
    return out;
}

void Seat::apply_pointer_config() {
    if (libinput_)
        for (const auto& d : libinput_->devices())
            if (d->pointer)
                configure_libinput(d->handle);
}

void Seat::configure_libinput(libinput_device* dev) {
    const Config& c = server.config;
    const bool touchpad = libinput_device_config_tap_get_finger_count(dev) > 0;
    const auto own = server.registry ? server.registry->device(libinput_device_get_name(dev)) : std::nullopt;
    const PointerSettings p = pointer_settings(c, own ? &*own : nullptr, touchpad);

    if (touchpad) {
        libinput_device_config_tap_set_enabled(dev, c.tap_to_click ? LIBINPUT_CONFIG_TAP_ENABLED
                                                                   : LIBINPUT_CONFIG_TAP_DISABLED);
        libinput_device_config_tap_set_drag_enabled(dev, c.tap_and_drag ? LIBINPUT_CONFIG_DRAG_ENABLED
                                                                        : LIBINPUT_CONFIG_DRAG_DISABLED);
        libinput_device_config_tap_set_drag_lock_enabled(dev, c.drag_lock
            ? LIBINPUT_CONFIG_DRAG_LOCK_ENABLED : LIBINPUT_CONFIG_DRAG_LOCK_DISABLED);
        libinput_device_config_tap_set_button_map(dev, LIBINPUT_CONFIG_TAP_MAP_LRM);
    }
    if (libinput_device_config_scroll_has_natural_scroll(dev))
        libinput_device_config_scroll_set_natural_scroll_enabled(dev, p.natural_scroll);
    if (libinput_device_config_dwt_is_available(dev))
        libinput_device_config_dwt_set_enabled(dev, c.disable_while_typing ? LIBINPUT_CONFIG_DWT_ENABLED
                                                                           : LIBINPUT_CONFIG_DWT_DISABLED);
    if (libinput_device_config_left_handed_is_available(dev))
        libinput_device_config_left_handed_set(dev, p.left_handed);
    if (libinput_device_config_middle_emulation_is_available(dev))
        libinput_device_config_middle_emulation_set_enabled(dev, c.middle_button_emulation
            ? LIBINPUT_CONFIG_MIDDLE_EMULATION_ENABLED : LIBINPUT_CONFIG_MIDDLE_EMULATION_DISABLED);
    if (libinput_device_config_accel_is_available(dev)) {
        libinput_device_config_accel_set_profile(dev, p.profile);
        libinput_device_config_accel_set_speed(dev, p.speed);
    }
}

void Seat::update_capabilities() {
    // Always a pointer (there is always a cursor) and a keyboard: keys come
    // from virtual keyboards too, which come and go with each use (vc, an
    // on-screen keyboard), and a capability that went takes the app's
    // wl_keyboard with it.
    uint32_t caps = wl::Seat::Pointer | wl::Seat::Keyboard;
    if (libinput_ && std::ranges::any_of(libinput_->devices(), [](const auto& d) { return d->touch; }))
        caps |= wl::Seat::Touch;
    server.wl->seat->set_capabilities(caps);
}

// --- keyboard ------------------------------------------------------------------

// What was typed, for snippet keywords: characters count, Backspace takes
// one back, anything that moves the caret starts over.
bool Seat::watch_keyword(KeyboardGroup& g, uint32_t keycode, xkb_keysym_t sym) {
    const uint32_t mods = g.keys.mod_mask();
    if (sym == XKB_KEY_Shift_L || sym == XKB_KEY_Shift_R || sym == XKB_KEY_Caps_Lock || sym == XKB_KEY_ISO_Level3_Shift)
        return false;
    if (mods & (WLR_MODIFIER_CTRL | WLR_MODIFIER_ALT | WLR_MODIFIER_LOGO)) {
        server.keywords.reset();
        return false;
    }
    if (sym == XKB_KEY_BackSpace) {
        server.keywords.backspace();
        return false;
    }
    const uint32_t c = xkb_state_key_get_utf32(g.keys.state(), keycode);
    if (c < 0x20 || c == 0x7f) {
        server.keywords.reset();
        return false;
    }
    auto hit = server.keywords.typed(char32_t(c));
    if (!hit || !server.ipc || !server.input_method || !server.input_method->takes_text_now())
        return false;
    // The app has all of the keyword but this last character.
    char last[8] = {};
    const int last_len = xkb_keysym_to_utf8(xkb_utf32_to_keysym(c), last, sizeof last) - 1;
    const size_t received = hit->second.size() - size_t(std::max(0, last_len));
    server.ipc->broadcast("shell", {{"event", "snippet.typed"}, {"snippet", hit->first}, {"delete", received}});
    return true;
}

void Seat::keyboard_enter(wl::Surface* surface) {
    server.keywords.reset();
    // Never enter with no keyboard (and so no keymap) while a real one exists.
    if (!seat_kb_)
        use_keyboard(physical_keyboard());
    const input::Keys& keys = seat_kb_->keys;
    std::vector<uint32_t> held;
    for (uint32_t k : keys.pressed())
        if (!consumed_[k])
            held.push_back(k);
    const input::Modifiers& m = keys.modifiers();
    server.wl->seat->keyboard_enter(surface, held, {m.depressed, m.latched, m.locked, m.group});
}

void Seat::clear_keyboard_focus() {
    server.wl->seat->keyboard_clear_focus();
}

const Keybind* Seat::find_binding(uint32_t mods, xkb_keysym_t sym) const {
    const Keybind* bind = find_keybind(server.config.keybinds, mods, sym);
    if (!bind || (server.locked && !bind->locked))
        return nullptr;
    // At the login screen nothing may be started as the greeter's user.
    if (server.config.greeter && bind->action != Action::SwitchVt)
        return nullptr;
    // The focused client asked for the keys; only switching VTs stays ours,
    // so there is always a way out.
    if (bind->action != Action::SwitchVt && shortcuts_inhibited())
        return nullptr;
    return bind;
}

bool Seat::shortcuts_inhibited() const {
    wl::Surface* focused = server.wl->seat->keyboard_focus();
    if (!focused)
        return false;
    const auto* i = server.wl->shortcut_inhibitors->for_surface(focused);
    return i && i->active;
}

void Seat::key(KeyboardGroup& g, uint32_t time_ms, uint32_t code, bool pressed) {
    const uint32_t keycode = code + 8;  // evdev → xkb
    // Match bindings on the unshifted and shifted symbol, so Super+Shift+E
    // is written with `e` and punctuation binds still work.
    for (int level = 0; level < 2; ++level)
        g.syms[level] = g.keys.sym_at(code, xkb_level_index_t(level));
    g.mods = g.keys.mod_mask();

    server.wl->idle_notifier->activity();

    const Keybind* bind = nullptr;
    if (pressed)
        for (xkb_keysym_t sym : g.syms)
            if ((bind = find_binding(g.mods, sym)))
                break;

    // An app's shortcut is held, not repeated.
    if (bind && bind->action == Action::Portal) {
        wl_event_source_timer_update(g.repeat_source, 0);
        consumed_[code] = true;
        portal_held_[code] = bind->arg;
        server.run_action(*bind);
        return;
    }

    if (bind && g.keys.repeat_delay > 0)
        wl_event_source_timer_update(g.repeat_source, g.keys.repeat_delay);
    else
        wl_event_source_timer_update(g.repeat_source, 0);

    if (bind) {
        consumed_[code] = true;
        server.run_action(*bind);
        return;
    }

    // The release of a key whose press ran a binding is not the client's.
    if (consumed_[code]) {
        if (!pressed) {
            consumed_[code] = false;
            if (auto held = portal_held_.find(code); held != portal_held_.end()) {
                server.portal_shortcut(held->second, false);
                portal_held_.erase(held);
            }
        }
        return;
    }

    if (server.switcher->active() && pressed) {
        consumed_[code] = true;
        server.switcher->key(g.syms[0]);
        return;
    }

    // The overview takes key presses; releases still reach the client, which
    // may have seen the press before the overview opened.
    if (server.overview->active() && pressed) {
        consumed_[code] = true;
        server.overview->key(g.syms[0]);
        return;
    }

    // The key that completes a snippet keyword never reaches the app: what it
    // did get of the keyword is taken back and the snippet typed instead, in
    // one go, with no keystroke still on its way to race the replacement.
    if (pressed && server.config.snippet_expansion && !server.keywords.empty() &&
        watch_keyword(g, keycode, g.syms[0])) {
        consumed_[code] = true;
        return;
    }

    // An input method composing text takes the keys first.
    if (server.input_method && server.input_method->forward_key(g.keys, g.owner, time_ms, code, pressed))
        return;

    use_keyboard(&g);
    server.wl->seat->keyboard_key(time_ms, code, pressed);
    // A virtual keyboard (an IME typing) speaks for itself only for its own
    // keys: the seat goes back to the real one, whose keymap every client
    // gets. Left on a virtual one, a client connecting later gets no keymap
    // (or none at all once it is gone), which crashes Chromium on focus.
    if (g.is_virtual)
        use_keyboard(physical_keyboard());
}

// Held on any keyboard: the seat speaks for the real one between a virtual
// keyboard's keys, but Super held on an on-screen keyboard still counts.
uint32_t Seat::held_modifiers() const {
    uint32_t mods = keyboards_->keys.mod_mask();
    for (const auto& g : virtual_keyboards_)
        mods |= g->keys.mod_mask();
    return mods;
}

void Seat::modifiers(KeyboardGroup& g) {
    if (!server.input_method || !server.input_method->forward_modifiers(g.keys, g.owner)) {
        const input::Modifiers& m = g.keys.modifiers();
        use_keyboard(&g);
        server.wl->seat->keyboard_modifiers({m.depressed, m.latched, m.locked, m.group});
        if (g.is_virtual)
            use_keyboard(physical_keyboard());
    }
    // Letting go of Alt picks the window the switcher is on.
    server.switcher->modifiers(g.keys.mod_mask());
    // A layout switch key (grp:...) on the physical keyboards.
    if (!g.is_virtual && layout() != last_layout_) {
        last_layout_ = layout();
        server.keyboard_layout_changed();
    }
}

int Seat::key_repeat(KeyboardGroup& g) {
    if (g.keys.repeat_rate <= 0)
        return 0;
    wl_event_source_timer_update(g.repeat_source, 1000 / g.keys.repeat_rate);
    for (xkb_keysym_t sym : g.syms)
        if (const Keybind* bind = find_binding(g.mods, sym)) {
            server.run_action(*bind);
            break;
        }
    return 0;
}

// --- pointer -------------------------------------------------------------------

// Over a fullscreen app the bar and the Dock wait under it, out of the way
// of its pointer (and of direct scanout); pushing the pointer against the
// top or bottom of that screen tells the shell to bring them over. Not
// while the app holds the pointer: a confined game never reaches the edge.
void Seat::reach_edge() {
    const char* edge = nullptr;
    Output* o = server.output_at(cursor->x, cursor->y);
    if (o && !active_constraint_ && o->fullscreen_bg->enabled) {
        // On a title bar brought out below the menu bar counts as the top:
        // both stay while it is used.
        int revealed = 0;
        for (View* v : server.views)
            if (v->output == o && v->visible())
                revealed = std::max(revealed, v->revealed_titlebar_bottom());
        if (cursor->y < o->box.y + std::max(1, revealed))
            edge = "top";
        else if (cursor->y >= o->box.y + o->box.height - 1)
            edge = "bottom";
    }
    const std::string at = edge ? o->wlr->name : "";
    if (edge == edge_reached_ && at == edge_output_)
        return;
    // Leaving says so too ("" on the screen it left), so a bar brought over
    // stays while the pointer rests on the edge.
    if (!edge_output_.empty() && at != edge_output_ && server.ipc)
        server.ipc->broadcast("outputs", {{"event", "output.edge"}, {"output", edge_output_}, {"edge", ""}});
    edge_reached_ = edge;
    edge_output_ = at;
    if (edge && server.ipc)
        server.ipc->broadcast("outputs", {{"event", "output.edge"}, {"output", at}, {"edge", edge}});
}

void Seat::motion(uint32_t time, wlr_input_device* device, double dx, double dy,
                  double dx_unaccel, double dy_unaccel) {
    wl::Surface* focused = server.wl->seat->pointer_focus();
    wl::Drag* drag = server.wl->data->drag();

    // time == 0: an internal refresh, not real motion.
    if (time) {
        server.wl->relative_pointers->send_motion(uint64_t(time) * 1000, dx, dy, dx_unaccel, dy_unaccel);

        activate_constraint(focused ? server.wl->pointer_constraints->for_surface(focused) : nullptr);

        if (active_constraint_ && mode != Mode::Move && mode != Mode::Resize &&
            active_constraint_->surface == focused) {
            if (Owner owner = Server::owner_of(active_constraint_->surface); owner.view) {
                double ox, oy;
                owner.view->surface_origin(ox, oy);
                double sx = cursor->x - ox, sy = cursor->y - oy, cx, cy;
                if (wlr_region_confine(active_constraint_->region.get(), sx, sy, sx + dx, sy + dy, &cx, &cy)) {
                    dx = cx - sx;
                    dy = cy - sy;
                }
                if (active_constraint_->type == wl::PointerConstraints::Type::Lock)
                    return;
            }
        }

        wlr_cursor_move(cursor, device, dx, dy);
        server.wl->idle_notifier->activity();
        if (server.config.shake_to_find && mode == Mode::Normal && shake_.feed(time, cursor->x, cursor->y))
            shake_grow();
        if (mode == Mode::Move && grab_view_)
            push_edge(dx);
    }

    server.drag_icons->set_position(int(std::lround(cursor->x)), int(std::lround(cursor->y)));

    if (server.overview->active()) {
        server.overview->motion(cursor->x, cursor->y);
        return;
    }
    if (server.switcher->active()) {
        server.switcher->motion(cursor->x, cursor->y);
        return;
    }

    if (mode == Mode::Move && grab_view_) {
        constexpr double kDragThreshold = 6;
        if (grab_unmaximize_) {
            if (std::hypot(cursor->x - grab_x_, cursor->y - grab_y_) < kDragThreshold)
                return;
            unmaximize_for_drag();
        }
        int nx = grab_geom_.x + int(std::lround(cursor->x - grab_x_));
        int ny = grab_geom_.y + int(std::lround(cursor->y - grab_y_));
        if (Output* o = server.output_at(cursor->x, cursor->y))
            geometry::snap(nx, ny, grab_view_->geom.width, grab_view_->geom.height, o->usable,
                           server.config.snap_distance);
        const int was_x = grab_view_->geom.x, was_y = grab_view_->geom.y;
        grab_view_->move_to(nx, ny);
        grab_view_->wobble(grab_view_->geom.x - was_x, grab_view_->geom.y - was_y, cursor->x, cursor->y);

        // Screen edges and corners offer to tile the window.
        Output* o = server.output_at(cursor->x, cursor->y);
        const bool tiling = (grab_view_->space && grab_view_->space->tiled) || grab_view_->layout_owned();
        const uint32_t zone = (o && server.config.snapping && !tiling && !edge_carried_)
            ? geometry::snap_zone(o->box, cursor->x, cursor->y, 4, 80) : 0;
        if (zone != snap_zone_) {
            snap_zone_ = zone;
            if (zone)
                server.snap_preview->show(geometry::snap_box(o->usable, zone, zone == WLR_EDGE_TOP ? 0 : server.config.snap_gap),
                                          grab_view_->tree, grab_view_->geom);
            else
                server.snap_preview->hide();
        }
        return;
    }
    if (mode == Mode::Resize && grab_view_) {
        wlr_box box = geometry::resize(grab_geom_, grab_edges_,
            int(std::lround(cursor->x - grab_x_)), int(std::lround(cursor->y - grab_y_)));
        // Pulled up past the menu bar, the top edge stops at it.
        if (const int top = grab_view_->below_bar(box.y); top > box.y && (grab_edges_ & WLR_EDGE_TOP)) {
            box.height -= top - box.y;
            box.y = top;
        }
        grab_view_->request_geometry(box);
        return;
    }

    if (time && mode == Mode::Normal)
        reach_edge();

    // A window riding a drag and drop follows the pointer, which looks
    // through it for where to drop.
    View* riding = nullptr;
    if (drag) {
        server.toplevel_drags->motion(cursor->x, cursor->y);
        riding = server.toplevel_drags->dragged();
    }
    Hit hit = server.hit_test(cursor->x, cursor->y, riding);
    // A drag and drop: what is under it hears of the drag, not the pointer.
    if (drag && !server.locked) {
        drag->motion(hit.surface, hit.sx, hit.sy, time ? time : now_ms());
        return;
    }

    // Among tiles (and in a secret space) focus follows the pointer, as in
    // caelestia; floating windows keep click to focus. Not while a panel or
    // menu has the keyboard.
    if (time && mode == Mode::Normal && !server.locked && hit.view && !hit.layer && hit.view->layout_owned() &&
        hit.view != server.focused_view) {
        wl::Surface* kf = server.wl->seat->keyboard_focus();
        if (!kf || (server.focused_view && kf == server.focused_view->surface()))
            server.focus_view(hit.view, false);
    }

    // A title-bar button held down: it shows pressed only while the pointer
    // stays on it, and nothing else gets the pointer meanwhile.
    if (press_bar_) {
        const auto part = hit.titlebar == press_bar_ ? int(press_bar_->part_at(hit.sx, hit.sy)) : 0;
        press_bar_->set_pressed(part == press_part_ ? Titlebar::Part(press_part_) : Titlebar::Part::None);
        return;
    }

    if (mode != Mode::Pressed) {
        if (ResizeZone zone = resize_zone(cursor->x, cursor->y, hit); zone.view) {
            set_titlebar_hover(nullptr, 0);
            server.wl->seat->pointer_clear_focus();
            wlr_cursor_set_xcursor(cursor, xcursor, wlr_xcursor_get_resize_name(wlr_edges(zone.edges)));
            return;
        }
        if (hit.backdrop) {
            set_titlebar_hover(nullptr, 0);
            server.wl->seat->pointer_clear_focus();
            set_default_cursor();
            return;
        }
        if (hit.titlebar) {
            set_titlebar_hover(hit.titlebar, int(hit.titlebar->part_at(hit.sx, hit.sy)));
            server.wl->seat->pointer_clear_focus();
            set_default_cursor();
            return;
        }
        set_titlebar_hover(nullptr, 0);
    }

    // Implicit grab: while a button is held, events stay with the surface
    // that got the press, even when the cursor leaves it.
    if (mode == Mode::Pressed && focused && hit.surface != focused) {
        if (Owner owner = Server::owner_of(focused)) {
            double ox, oy;
            if (owner.layer) {
                ox = owner.layer->tree->x;
                oy = owner.layer->tree->y;
            } else {
                owner.view->surface_origin(ox, oy);
            }
            hit.surface = focused;
            hit.view = owner.view;
            hit.layer = owner.layer;
            hit.sx = cursor->x - ox;
            hit.sy = cursor->y - oy;
        }
    }

    if (!hit.surface)
        set_default_cursor();
    pointer_focus(hit.view, hit.surface, hit.sx, hit.sy, time);
}

// The pointer to a point in the layout (a tablet, a touchscreen pointer, a
// virtual pointer's absolute motion).
void Seat::motion_absolute(uint32_t time, double lx, double ly) {
    if (!time) {  // virtual pointers send time 0 and expect a warp
        wlr_cursor_warp_closest(cursor, nullptr, lx, ly);
        motion(0, nullptr, 0, 0, 0, 0);
        return;
    }
    const double dx = lx - cursor->x, dy = ly - cursor->y;
    motion(time, nullptr, dx, dy, dx, dy);
}

void Seat::pointer_focus(View*, wl::Surface* surface, double sx, double sy, uint32_t time) {
    wl::Seat& ws = *server.wl->seat;
    if (!surface) {
        if (ws.pointer_focus())
            set_cursor_surface(nullptr, 0, 0);
        ws.pointer_clear_focus();
        return;
    }
    if (!time)
        time = now_ms();
    // Entering another surface: its own image comes with its enter.
    if (surface != ws.pointer_focus()) {
        set_cursor_surface(nullptr, 0, 0);
        set_default_cursor();
        ws.pointer_enter(surface, sx, sy);
    }
    ws.pointer_motion(time, sx, sy);
}

void Seat::button(const ButtonEvent& event) {
    const ButtonEvent* e = &event;
    if (e->pressed)
        server.keywords.reset();  // a click moves the caret
    server.wl->idle_notifier->activity();

    // A drag and drop ends with the button: dropped where it is.
    if (wl::Drag* drag = server.wl->data->drag()) {
        if (!e->pressed)
            drag->drop(e->time_ms);
        mode = Mode::Normal;
        return;
    }

    // A press on the overview is the overview's, and so is its release, even
    // when the overview has gone by then.
    const bool pressed = e->pressed;
    if (server.overview->active() || (!pressed && overview_press_)) {
        overview_press_ = pressed;
        server.overview->button(cursor->x, cursor->y, e->button, pressed);
        return;
    }
    if (server.switcher->active() || (!pressed && switcher_press_)) {
        switcher_press_ = pressed;
        server.switcher->button(cursor->x, cursor->y, pressed);
        return;
    }

    if (e->pressed) {
        mode = Mode::Pressed;
        if (Output* o = server.output_at(cursor->x, cursor->y))
            server.focused_output = o;
        if (server.locked) {
            server.wl->seat->pointer_button(e->time_ms, e->button, true);
            return;
        }

        Hit hit = server.hit_test(cursor->x, cursor->y);

        // Where the press landed, for the shell: its panels close on a press
        // anywhere else, as macOS's do (focus isn't a reliable sign of that:
        // a click on the window already focused changes nothing).
        if (server.ipc) {
            const std::string ns = hit.layer ? hit.layer->ls->name_space() : "";
            server.ipc->broadcast("shell", {{"event", "pointer.pressed"}, {"namespace", ns}});
        }

        // Frame edges and title bars belong to atrium, not the client.
        if (ResizeZone zone = resize_zone(cursor->x, cursor->y, hit); zone.view) {
            server.focus_view(zone.view);
            if (e->button == BTN_LEFT)
                begin_resize(zone.view, zone.edges);
            return;
        }
        if (hit.titlebar && titlebar_button(*e, hit))
            return;
        // Clicking the dimmed screen puts a secret space away.
        if (hit.backdrop) {
            server.hide_secret();
            return;
        }

        // Click to focus, and a click raises: the desktop model, not the tiling one.
        if (hit.view && (!hit.view->unmanaged() || hit.view->wants_focus()))
            server.focus_view(hit.view);
        else if (hit.layer && hit.layer->ls->current().keyboard_interactive)
            server.focus_layer(hit.layer);

        // Mod + drag moves (left) or resizes (right) from anywhere in a window.
        const uint32_t mods = held_modifiers();
        if (hit.view && !hit.view->unmanaged() && clean_mods(mods) == server.config.mod) {
            if (e->button == BTN_LEFT) {
                begin_move(hit.view);
                return;
            }
            if (e->button == BTN_RIGHT) {
                begin_resize(hit.view, geometry::nearest_corner(hit.view->geom, cursor->x, cursor->y));
                return;
            }
        }
    } else {
        if (press_bar_) {
            titlebar_button(*e, server.hit_test(cursor->x, cursor->y));
            return;
        }
        if (!server.locked && (mode == Mode::Move || mode == Mode::Resize)) {
            // The grab ate the press; the release ends it and is ours too.
            // Dropped over a snap zone, the window takes it.
            View* dropped = mode == Mode::Move ? grab_view_ : nullptr;
            const uint32_t zone = snap_zone_;
            cancel_grab();
            if (dropped && dropped->tiled())
                server.tile_drop(dropped, cursor->x, cursor->y);  // trade places, or back to its slot
            else if (dropped && dropped->layout_owned())
                dropped->fit_secret(false);  // back to its frame
            else if (dropped && zone)
                dropped->snap(zone);
            server.wl->seat->pointer_clear_focus();
            set_default_cursor();
            refresh_pointer();
            return;
        }
        mode = Mode::Normal;
    }
    server.wl->seat->pointer_button(e->time_ms, e->button, e->pressed);
}

// --- title bars and frame edges -----------------------------------------------------

Seat::ResizeZone Seat::resize_zone(double lx, double ly, const Hit& hit) const {
    constexpr int kBand = 8;    // resize band just outside the frame
    constexpr int kCorner = 16; // along an edge, this close to a corner resizes both ways
    if (server.locked)
        return {};
    // Panels, docks and menus above windows keep their clicks.
    if (hit.layer && hit.layer->ls->current().layer >= ZWLR_LAYER_SHELL_V1_LAYER_TOP)
        return {};

    // `views` is in stacking order: the first window under the point hides
    // every band below it.
    // Under a secret space's backdrop only the secret windows are reachable.
    const Space* only = hit.backdrop ? hit.backdrop : nullptr;
    for (View* v : server.views) {
        if (!v->visible() || (only && v->space != only))
            continue;
        const wlr_box& g = v->geom;
        if (lx >= g.x && lx < g.x + g.width && ly >= g.y && ly < g.y + g.height)
            return {};
        // Apps drawing their own frame get the band too: Electron's has no
        // resize edges on Wayland.
        if (v->fullscreen || v->maximized || v->unmanaged() || v->splash() || v->layout_owned() || v->fixed_size())
            continue;
        if (lx < g.x - kBand || lx >= g.x + g.width + kBand || ly < g.y - kBand || ly >= g.y + g.height + kBand)
            continue;
        uint32_t edges = 0;
        if (lx < g.x + kCorner) edges |= WLR_EDGE_LEFT;
        else if (lx >= g.x + g.width - kCorner) edges |= WLR_EDGE_RIGHT;
        if (ly < g.y + kCorner) edges |= WLR_EDGE_TOP;
        else if (ly >= g.y + g.height - kCorner) edges |= WLR_EDGE_BOTTOM;
        // A point beside the frame counts only for the side it is on.
        if (lx >= g.x && lx < g.x + g.width && !(ly < g.y || ly >= g.y + g.height))
            edges &= WLR_EDGE_TOP | WLR_EDGE_BOTTOM;
        if (edges)
            return {v, edges};
    }
    return {};
}

void Seat::set_titlebar_hover(Titlebar* bar, int part) {
    if (hover_bar_ && hover_bar_ != bar)
        hover_bar_->set_hover(Titlebar::Part::None);
    hover_bar_ = bar;
    if (bar)
        bar->set_hover(Titlebar::Part(part));
}

// Returns true when the event was the title bar's.
bool Seat::titlebar_button(const ButtonEvent& event, const Hit& hit) {
    const ButtonEvent* e = &event;
    using Part = Titlebar::Part;
    if (e->pressed) {
        const Part part = hit.titlebar->part_at(hit.sx, hit.sy);
        if (e->button != BTN_LEFT) {
            server.focus_view(hit.view);
            if (e->button == BTN_RIGHT && part == Part::Bar)
                server.show_window_menu(hit.view, cursor->x, cursor->y);
            return true;
        }
        if (part == Part::Bar) {
            server.focus_view(hit.view);
            // Double-click zooms, like macOS.
            const bool twice = last_bar_click_view_ == hit.view && e->time_ms - last_bar_click_ms_ < 400;
            last_bar_click_view_ = twice ? nullptr : hit.view;
            last_bar_click_ms_ = e->time_ms;
            // A tile or a secret window moves only with Mod + drag.
            if (hit.view->layout_owned())
                ;
            else if (twice && !hit.view->fullscreen)
                hit.view->set_maximized(!hit.view->maximized);
            else
                begin_move(hit.view);
            return true;
        }
        if (part == Part::None)
            return false;
        // Buttons act on release, and only if still over them.
        press_bar_ = hit.titlebar;
        press_part_ = int(part);
        press_bar_->set_pressed(part);
        server.wl->seat->pointer_clear_focus();
        return true;
    }

    Titlebar* bar = press_bar_;
    const Part part = Part(press_part_);
    press_bar_ = nullptr;
    press_part_ = 0;
    mode = Mode::Normal;
    bar->set_pressed(Part::None);
    if (hit.titlebar != bar || bar->part_at(hit.sx, hit.sy) != part) {
        refresh_pointer();
        return true;
    }
    View& v = bar->view();
    switch (part) {
    case Part::Close: v.close(); break;
    case Part::Minimize: if (!v.layout_owned()) v.set_minimized(true); break;
    // Green is full screen, as on a Mac: the window takes the whole screen,
    // menu bar, title bar and Dock gone. With Alt (Option) it zooms instead.
    case Part::Maximize:
        if (held_modifiers() & WLR_MODIFIER_ALT) {
            if (!v.fullscreen && !v.layout_owned())
                v.set_maximized(!v.maximized);
        } else {
            v.set_fullscreen(!v.fullscreen);
        }
        break;
    default: break;
    }
    refresh_pointer();
    return true;
}

void Seat::axis(const AxisEvent& event) {
    const AxisEvent* e = &event;
    server.wl->idle_notifier->activity();
    // Mod + scroll steps through spaces, with Alt taking the focused window
    // along, as caelestia's Super + wheel does. A wheel steps once a notch; a
    // touchpad once per stretch of scrolling.
    const uint32_t mods = clean_mods(held_modifiers());
    if (!server.locked && e->orientation == WL_POINTER_AXIS_VERTICAL_SCROLL &&
        (mods == server.config.mod || mods == (server.config.mod | WLR_MODIFIER_ALT))) {
        const bool wheel = e->source == WL_POINTER_AXIS_SOURCE_WHEEL && e->value120 != 0;
        space_scroll_ += wheel ? e->value120 / 120.0 : e->delta / 40.0;
        while (std::abs(space_scroll_) >= 1) {
            const int step = space_scroll_ > 0 ? 1 : -1;
            space_scroll_ -= step;
            // Down goes back, as caelestia binds it (mouse_down: workspace -1).
            const bool forward = step < 0;
            if (mods & WLR_MODIFIER_ALT)
                server.run_action({.mods = 0, .sym = 0, .action = forward ? Action::MoveToSpaceNext : Action::MoveToSpacePrev});
            else
                server.step_space(forward ? 1 : -1);
        }
        return;
    }
    space_scroll_ = 0;
    server.wl->seat->pointer_axis(e->time_ms, e->orientation, e->delta, e->value120, wl::Seat::AxisSource(e->source),
                                  e->inverted);
}

// --- interactive move / resize ---------------------------------------------------

void Seat::begin_move(View* view) {
    if (mode == Mode::Move || mode == Mode::Resize || view->fullscreen || view->unmanaged())
        return;
    grab_view_ = view;
    grab_x_ = cursor->x;
    grab_y_ = cursor->y;
    grab_geom_ = view->geom;
    // A maximized window stays put until the pointer really drags it: a
    // click (or the first half of a double-click) must not restore it.
    grab_unmaximize_ = (view->maximized || view->snapped) && !view->layout_owned();
    snap_zone_ = 0;
    edge_push_ = 0;
    edge_carried_ = false;
    mode = Mode::Move;
    server.wl->seat->pointer_clear_focus();
    wlr_cursor_set_xcursor(cursor, xcursor, "grabbing");
}

// Dragging a maximized window restores its size under the cursor, keeping the
// same relative grab point horizontally.
void Seat::unmaximize_for_drag() {
    View* view = grab_view_;
    grab_unmaximize_ = false;
    const wlr_box before = view->geom;
    const double fx = before.width > 0 ? (grab_x_ - before.x) / before.width : 0.5;
    if (view->maximized)
        view->set_maximized(false);
    else
        view->unsnap(true);
    const int nx = int(std::lround(grab_x_ - fx * view->restore.width));
    view->move_to(nx, before.y);
    view->geom.width = view->restore.width;  // grab math uses the size it is heading to
    view->geom.height = view->restore.height;
    grab_geom_ = view->geom;
}

void Seat::begin_resize(View* view, uint32_t edges) {
    // Tiles take the size the layout gives them.
    if (mode == Mode::Move || mode == Mode::Resize || view->fullscreen || view->unmanaged() || !edges ||
        view->layout_owned())
        return;
    if (view->maximized)
        view->set_maximized(false, false);

    grab_view_ = view;
    grab_x_ = cursor->x;
    grab_y_ = cursor->y;
    grab_geom_ = view->geom;
    grab_edges_ = edges;
    mode = Mode::Resize;
    view->begin_resize(edges);
    server.wl->seat->pointer_clear_focus();
    wlr_cursor_set_xcursor(cursor, xcursor, wlr_xcursor_get_resize_name(wlr_edges(edges)));
}

void Seat::push_edge(double dx) {
    // A deliberate shove, well past the touch that offers to snap. The edge is
    // a band a few pixels wide: a hand on a mouse (or vc's pointer) jitters.
    constexpr double kPush = 300, kBand = 4;
    wlr_box all;
    wlr_output_layout_get_box(server.output_layout, nullptr, &all);
    const int dir = cursor->x < all.x + kBand ? -1 : cursor->x >= all.x + all.width - kBand ? 1 : 0;
    if (!dir) {
        edge_push_ = 0, edge_carried_ = false;
        return;
    }
    if (dx * dir <= 0)
        return;
    edge_push_ += std::abs(dx);
    if (edge_push_ < kPush || grab_view_->layout_owned())
        return;
    edge_push_ = 0;
    if (server.carry_to_space(grab_view_, dir)) {
        edge_carried_ = true;
        snap_zone_ = 0;
        server.snap_preview->hide();
    }
}

void Seat::cancel_grab() {
    if (grab_view_ && mode == Mode::Resize)
        grab_view_->end_resize();
    snap_zone_ = 0;
    if (server.snap_preview)
        server.snap_preview->hide();
    grab_view_ = nullptr;
    grab_unmaximize_ = false;
    grab_edges_ = 0;
    mode = Mode::Normal;
}

void Seat::titlebar_gone(Titlebar* bar) {
    if (hover_bar_ == bar)
        hover_bar_ = nullptr;
    if (press_bar_ == bar) {
        press_bar_ = nullptr;
        mode = Mode::Normal;
    }
}

void Seat::view_unmapped(View* view) {
    if (grab_view_ == view)
        cancel_grab();
    if (hover_bar_ && &hover_bar_->view() == view)
        hover_bar_ = nullptr;
    if (press_bar_ && &press_bar_->view() == view) {
        press_bar_ = nullptr;
        mode = Mode::Normal;
    }
    if (last_bar_click_view_ == view)
        last_bar_click_view_ = nullptr;
}

// --- pointer constraints (games, remote desktops) ------------------------------------

void Seat::activate_constraint(wl::PointerConstraints::Constraint* c) {
    if (active_constraint_ == c)
        return;
    if (active_constraint_)
        server.wl->pointer_constraints->deactivate(active_constraint_);
    active_constraint_ = c;
    if (c)
        server.wl->pointer_constraints->activate(c);
}

void Seat::warp_to_constraint_hint() {
    auto* c = active_constraint_;
    if (!c->cursor_hint)
        return;
    Owner owner = Server::owner_of(c->surface);
    if (!owner.view)
        return;
    double ox, oy;
    owner.view->surface_origin(ox, oy);
    wlr_cursor_warp(cursor, nullptr, ox + c->cursor_hint->first, oy + c->cursor_hint->second);
}

} // namespace atrium
