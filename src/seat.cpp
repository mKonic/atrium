#include "seat.hpp"

#include "geometry.hpp"
#include "layer_surface.hpp"
#include "output.hpp"
#include "overview.hpp"
#include "server.hpp"
#include "space.hpp"
#include "snap_preview.hpp"
#include "titlebar.hpp"
#include "view.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <ctime>

namespace atrium {

namespace {

xkb_keysym_t sym_at_level(xkb_keymap* keymap, xkb_keycode_t key, xkb_layout_index_t layout,
                          xkb_level_index_t level) {
    const xkb_keysym_t* syms;
    int n = xkb_keymap_key_get_syms_by_level(keymap, key, layout, level, &syms);
    return n ? syms[0] : XKB_KEY_NoSymbol;
}

xkb_keymap* compile_keymap(const Config& c) {
    auto opt = [](const std::string& s) { return s.empty() ? nullptr : s.c_str(); };
    xkb_rule_names names{};
    names.rules = opt(c.xkb_rules);
    names.model = opt(c.xkb_model);
    names.layout = opt(c.xkb_layout);
    names.variant = opt(c.xkb_variant);
    names.options = opt(c.xkb_options);

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

KeyboardGroup::KeyboardGroup(Seat& s, bool is_virtual) : seat(s) {
    group = wlr_keyboard_group_create();
    group->data = this;

    xkb_keymap* keymap = compile_keymap(seat.server.config);
    wlr_keyboard_set_keymap(&group->keyboard, keymap);
    xkb_keymap_unref(keymap);
    wlr_keyboard_set_repeat_info(&group->keyboard, seat.server.config.repeat_rate,
                                 seat.server.config.repeat_delay);

    key.connect(&group->keyboard.events.key, [this](wlr_keyboard_key_event* e) { seat.key(*this, e); });
    modifiers.connect(&group->keyboard.events.modifiers, [this](void*) { seat.modifiers(*this); });
    repeat_source = wl_event_loop_add_timer(seat.server.loop, [](void* data) {
        auto* g = static_cast<KeyboardGroup*>(data);
        return g->seat.key_repeat(*g);
    }, this);

    // The seat has one keyboard; the physical group is it.
    if (!is_virtual)
        wlr_seat_set_keyboard(seat.wlr, &group->keyboard);
}

KeyboardGroup::~KeyboardGroup() {
    wl_event_source_remove(repeat_source);
    key.disconnect();
    modifiers.disconnect();
    destroy.disconnect();
    wlr_keyboard_group_destroy(group);
}

// --- seat ----------------------------------------------------------------------

struct Seat::Constraint {
    explicit Constraint(wlr_pointer_constraint_v1* c) : wlr(c) {}
    wlr_pointer_constraint_v1* wlr;
    Listener<> destroy;
};

Seat::Seat(Server& srv) : server(srv) {
    wlr = wlr_seat_create(server.display, "seat0");
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
    cursor_button_.connect(&cursor->events.button, [this](wlr_pointer_button_event* e) { button(e); });
    cursor_axis_.connect(&cursor->events.axis, [this](wlr_pointer_axis_event* e) { axis(e); });
    cursor_frame_.connect(&cursor->events.frame, [this](void*) { wlr_seat_pointer_notify_frame(wlr); });

    request_cursor_.connect(&wlr->events.request_set_cursor,
        [this](wlr_seat_pointer_request_set_cursor_event* e) {
            // While we own the pointer (move/resize) the client's image waits.
            if (mode != Mode::Normal && mode != Mode::Pressed)
                return;
            if (e->seat_client == wlr->pointer_state.focused_client)
                wlr_cursor_set_surface(cursor, e->surface, e->hotspot_x, e->hotspot_y);
        });
    request_cursor_shape_.connect(&server.cursor_shape_manager->events.request_set_shape,
        [this](wlr_cursor_shape_manager_v1_request_set_shape_event* e) {
            if (mode != Mode::Normal && mode != Mode::Pressed)
                return;
            if (e->seat_client == wlr->pointer_state.focused_client)
                wlr_cursor_set_xcursor(cursor, xcursor, wlr_cursor_shape_v1_name(e->shape));
        });
    request_selection_.connect(&wlr->events.request_set_selection,
        [this](wlr_seat_request_set_selection_event* e) { wlr_seat_set_selection(wlr, e->source, e->serial); });
    request_primary_selection_.connect(&wlr->events.request_set_primary_selection,
        [this](wlr_seat_request_set_primary_selection_event* e) {
            wlr_seat_set_primary_selection(wlr, e->source, e->serial);
        });
    request_start_drag_.connect(&wlr->events.request_start_drag,
        [this](wlr_seat_request_start_drag_event* e) {
            if (wlr_seat_validate_pointer_grab_serial(wlr, e->origin, e->serial))
                wlr_seat_start_pointer_drag(wlr, e->drag, e->serial);
            else
                wlr_data_source_destroy(e->drag->source);
        });
    start_drag_.connect(&wlr->events.start_drag, [this](wlr_drag* drag) {
        if (!drag->icon)
            return;
        drag->icon->data = &wlr_scene_drag_icon_create(server.drag_icons, drag->icon)->node;
        struct Watch {
            Listener<> destroy;
        };
        auto* watch = new Watch;
        watch->destroy.connect(&drag->icon->events.destroy, [this, watch](void*) {
            // Focus enter is not sent during a drag; restore it now.
            server.focus_top();
            refresh_pointer();
            delete watch;
        });
    });

    new_constraint_.connect(&server.pointer_constraints->events.new_constraint,
        [this](wlr_pointer_constraint_v1* c) { new_constraint(c); });

    new_input_.connect(&server.backend->events.new_input, [this](wlr_input_device* d) { new_input(d); });
    new_virtual_keyboard_.connect(&server.virtual_keyboard_manager->events.new_virtual_keyboard,
        [this](wlr_virtual_keyboard_v1* vk) {
            // Its own group: synthetic input never shares modifier state with
            // the physical keyboard.
            auto group = std::make_unique<KeyboardGroup>(*this, true);
            KeyboardGroup* g = group.get();
            wlr_keyboard_set_keymap(&vk->keyboard, g->group->keyboard.keymap);
            g->destroy.connect(&vk->keyboard.base.events.destroy, [this, g](void*) {
                std::erase_if(virtual_keyboards_, [g](auto& p) { return p.get() == g; });
            });
            wlr_keyboard_group_add_keyboard(g->group, &vk->keyboard);
            virtual_keyboards_.push_back(std::move(group));
            wlr_seat_set_capabilities(wlr, wlr->capabilities | WL_SEAT_CAPABILITY_KEYBOARD);
        });
    new_virtual_pointer_.connect(&server.virtual_pointer_manager->events.new_virtual_pointer,
        [this](wlr_virtual_pointer_v1_new_pointer_event* e) {
            wlr_input_device* dev = &e->new_pointer->pointer.base;
            wlr_cursor_attach_input_device(cursor, dev);
            if (e->suggested_output)
                wlr_cursor_map_input_to_output(cursor, dev, e->suggested_output);
            wlr_seat_set_capabilities(wlr, wlr->capabilities | WL_SEAT_CAPABILITY_POINTER);
        });

    keyboards_ = std::make_unique<KeyboardGroup>(*this, false);
}

Seat::~Seat() {
    // Off every signal before the objects carrying them go away.
    new_input_.disconnect();
    new_virtual_keyboard_.disconnect();
    new_virtual_pointer_.disconnect();
    cursor_motion_.disconnect();
    cursor_motion_absolute_.disconnect();
    cursor_button_.disconnect();
    cursor_axis_.disconnect();
    cursor_frame_.disconnect();
    request_cursor_.disconnect();
    request_cursor_shape_.disconnect();
    request_selection_.disconnect();
    request_primary_selection_.disconnect();
    request_start_drag_.disconnect();
    start_drag_.disconnect();
    new_constraint_.disconnect();
    constraints_.clear();
    pointers_.clear();
    virtual_keyboards_.clear();
    keyboards_.reset();
    wlr_xcursor_manager_destroy(xcursor);
    wlr_cursor_destroy(cursor);
}

void Seat::apply_keyboard_config() {
    xkb_keymap* keymap = compile_keymap(server.config);
    auto apply = [&](KeyboardGroup& g) {
        wlr_keyboard_set_keymap(&g.group->keyboard, keymap);
        wlr_keyboard_set_repeat_info(&g.group->keyboard, server.config.repeat_rate,
                                     server.config.repeat_delay);
    };
    apply(*keyboards_);
    for (auto& g : virtual_keyboards_)
        apply(*g);
    xkb_keymap_unref(keymap);
}

void Seat::apply_cursor_theme() {
    if (xcursor)
        wlr_xcursor_manager_destroy(xcursor);
    const Config& c = server.config;
    const char* theme = c.cursor_theme.empty() ? getenv("XCURSOR_THEME") : c.cursor_theme.c_str();
    xcursor = wlr_xcursor_manager_create(theme, c.cursor_size);
    setenv("XCURSOR_SIZE", std::to_string(c.cursor_size).c_str(), 1);
    if (theme)
        setenv("XCURSOR_THEME", theme, 1);
}

void Seat::set_default_cursor() {
    wlr_cursor_set_xcursor(cursor, xcursor, "default");
}

// --- devices -----------------------------------------------------------------

void Seat::new_input(wlr_input_device* device) {
    switch (device->type) {
    case WLR_INPUT_DEVICE_KEYBOARD:
        add_keyboard(wlr_keyboard_from_input_device(device));
        break;
    case WLR_INPUT_DEVICE_POINTER:
        add_pointer(wlr_pointer_from_input_device(device));
        break;
    default:
        break;  // touch, tablets, switches: not yet
    }
    update_capabilities();
}

void Seat::add_keyboard(wlr_keyboard* keyboard) {
    wlr_keyboard_set_keymap(keyboard, keyboards_->group->keyboard.keymap);
    wlr_keyboard_group_add_keyboard(keyboards_->group, keyboard);
}

void Seat::add_pointer(wlr_pointer* pointer) {
    if (wlr_input_device_is_libinput(&pointer->base))
        if (libinput_device* dev = wlr_libinput_get_device_handle(&pointer->base))
            configure_libinput(dev);
    wlr_cursor_attach_input_device(cursor, &pointer->base);

    // Remembered so settings changes can reach devices already plugged in.
    auto dev = std::make_unique<PointerDevice>(pointer);
    PointerDevice* raw = dev.get();
    raw->destroy.connect(&pointer->base.events.destroy, [this, raw](void*) {
        std::erase_if(pointers_, [raw](auto& p) { return p.get() == raw; });
    });
    pointers_.push_back(std::move(dev));
}

void Seat::apply_pointer_config() {
    for (auto& p : pointers_)
        if (wlr_input_device_is_libinput(&p->wlr->base))
            if (libinput_device* dev = wlr_libinput_get_device_handle(&p->wlr->base))
                configure_libinput(dev);
}

void Seat::configure_libinput(libinput_device* dev) {
    const Config& c = server.config;
    const bool touchpad = libinput_device_config_tap_get_finger_count(dev) > 0;

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
        libinput_device_config_scroll_set_natural_scroll_enabled(dev,
            touchpad ? c.touchpad_natural_scroll : c.natural_scroll);
    if (libinput_device_config_dwt_is_available(dev))
        libinput_device_config_dwt_set_enabled(dev, c.disable_while_typing ? LIBINPUT_CONFIG_DWT_ENABLED
                                                                           : LIBINPUT_CONFIG_DWT_DISABLED);
    if (libinput_device_config_left_handed_is_available(dev))
        libinput_device_config_left_handed_set(dev, c.left_handed);
    if (libinput_device_config_middle_emulation_is_available(dev))
        libinput_device_config_middle_emulation_set_enabled(dev, c.middle_button_emulation
            ? LIBINPUT_CONFIG_MIDDLE_EMULATION_ENABLED : LIBINPUT_CONFIG_MIDDLE_EMULATION_DISABLED);
    if (libinput_device_config_accel_is_available(dev)) {
        libinput_device_config_accel_set_profile(dev, c.accel_profile);
        libinput_device_config_accel_set_speed(dev, c.accel_speed);
    }
}

void Seat::update_capabilities() {
    // Always advertise a pointer: there is always a cursor.
    uint32_t caps = WL_SEAT_CAPABILITY_POINTER;
    if (!wl_list_empty(&keyboards_->group->devices))
        caps |= WL_SEAT_CAPABILITY_KEYBOARD;
    wlr_seat_set_capabilities(wlr, caps);
}

// --- keyboard ------------------------------------------------------------------

void Seat::keyboard_enter(wlr_surface* surface) {
    wlr_keyboard* kb = wlr_seat_get_keyboard(wlr);
    if (!kb) {
        wlr_seat_keyboard_notify_enter(wlr, surface, nullptr, 0, nullptr);
        return;
    }
    uint32_t held[WLR_KEYBOARD_KEYS_CAP];
    size_t n = 0;
    for (size_t i = 0; i < kb->num_keycodes; ++i)
        if (!consumed_[kb->keycodes[i]])
            held[n++] = kb->keycodes[i];
    wlr_seat_keyboard_notify_enter(wlr, surface, held, n, &kb->modifiers);
}

void Seat::clear_keyboard_focus() {
    wlr_seat_keyboard_notify_clear_focus(wlr);
}

const Keybind* Seat::find_binding(uint32_t mods, xkb_keysym_t sym) const {
    return find_keybind(server.config.keybinds, mods, sym);
}

void Seat::key(KeyboardGroup& g, wlr_keyboard_key_event* e) {
    const uint32_t keycode = e->keycode + 8;  // evdev → xkb
    wlr_keyboard* kb = &g.group->keyboard;
    xkb_layout_index_t layout = xkb_state_key_get_layout(kb->xkb_state, keycode);

    // Match bindings on the unshifted and shifted symbol, so Super+Shift+E
    // is written with `e` and punctuation binds still work.
    for (int level = 0; level < 2; ++level)
        g.syms[level] = sym_at_level(kb->keymap, keycode, layout, level);
    g.mods = wlr_keyboard_get_modifiers(kb);

    wlr_idle_notifier_v1_notify_activity(server.idle_notifier, wlr);

    const Keybind* bind = nullptr;
    if (!server.locked && e->state == WL_KEYBOARD_KEY_STATE_PRESSED)
        for (xkb_keysym_t sym : g.syms)
            if ((bind = find_binding(g.mods, sym)))
                break;

    if (bind && kb->repeat_info.delay > 0)
        wl_event_source_timer_update(g.repeat_source, kb->repeat_info.delay);
    else
        wl_event_source_timer_update(g.repeat_source, 0);

    if (bind) {
        consumed_[e->keycode] = true;
        server.run_action(*bind);
        return;
    }

    // The release of a key whose press ran a binding is not the client's.
    if (consumed_[e->keycode]) {
        if (e->state == WL_KEYBOARD_KEY_STATE_RELEASED)
            consumed_[e->keycode] = false;
        return;
    }

    // The overview takes key presses; releases still reach the client, which
    // may have seen the press before the overview opened.
    if (server.overview->active() && e->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
        consumed_[e->keycode] = true;
        server.overview->key(g.syms[0]);
        return;
    }

    wlr_seat_set_keyboard(wlr, kb);
    wlr_seat_keyboard_notify_key(wlr, e->time_msec, e->keycode, e->state);
}

void Seat::modifiers(KeyboardGroup& g) {
    wlr_seat_set_keyboard(wlr, &g.group->keyboard);
    wlr_seat_keyboard_notify_modifiers(wlr, &g.group->keyboard.modifiers);
}

int Seat::key_repeat(KeyboardGroup& g) {
    wlr_keyboard* kb = &g.group->keyboard;
    if (server.locked || kb->repeat_info.rate <= 0)
        return 0;
    wl_event_source_timer_update(g.repeat_source, 1000 / kb->repeat_info.rate);
    for (xkb_keysym_t sym : g.syms)
        if (const Keybind* bind = find_binding(g.mods, sym)) {
            server.run_action(*bind);
            break;
        }
    return 0;
}

// --- pointer -------------------------------------------------------------------

void Seat::motion(uint32_t time, wlr_input_device* device, double dx, double dy,
                  double dx_unaccel, double dy_unaccel) {
    wlr_surface* focused = wlr->pointer_state.focused_surface;

    // time == 0: an internal refresh, not real motion.
    if (time) {
        wlr_relative_pointer_manager_v1_send_relative_motion(server.relative_pointer_manager, wlr,
            uint64_t(time) * 1000, dx, dy, dx_unaccel, dy_unaccel);

        activate_constraint(focused ? wlr_pointer_constraints_v1_constraint_for_surface(
                                          server.pointer_constraints, focused, wlr)
                                    : nullptr);

        if (active_constraint_ && mode != Mode::Move && mode != Mode::Resize &&
            active_constraint_->surface == focused) {
            if (Owner owner = Server::owner_of(active_constraint_->surface); owner.view) {
                double ox, oy;
                owner.view->surface_origin(ox, oy);
                double sx = cursor->x - ox, sy = cursor->y - oy, cx, cy;
                if (wlr_region_confine(&active_constraint_->region, sx, sy, sx + dx, sy + dy, &cx, &cy)) {
                    dx = cx - sx;
                    dy = cy - sy;
                }
                if (active_constraint_->type == WLR_POINTER_CONSTRAINT_V1_LOCKED)
                    return;
            }
        }

        wlr_cursor_move(cursor, device, dx, dy);
        wlr_idle_notifier_v1_notify_activity(server.idle_notifier, wlr);
    }

    wlr_scene_node_set_position(&server.drag_icons->node, int(std::lround(cursor->x)),
                                int(std::lround(cursor->y)));

    if (server.overview->active()) {
        server.overview->motion(cursor->x, cursor->y);
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
        grab_view_->move_to(nx, ny);

        // Screen edges and corners offer to tile the window.
        Output* o = server.output_at(cursor->x, cursor->y);
        const uint32_t zone = (o && server.config.snapping)
            ? geometry::snap_zone(o->box, cursor->x, cursor->y, 4, 80) : 0;
        if (zone != snap_zone_) {
            snap_zone_ = zone;
            if (zone)
                server.snap_preview->show(geometry::snap_box(o->usable, zone, zone == WLR_EDGE_TOP ? 0 : server.config.snap_gap),
                                          &grab_view_->tree->node, grab_view_->geom);
            else
                server.snap_preview->hide();
        }
        return;
    }
    if (mode == Mode::Resize && grab_view_) {
        grab_view_->request_geometry(geometry::resize(grab_geom_, grab_edges_,
            int(std::lround(cursor->x - grab_x_)), int(std::lround(cursor->y - grab_y_))));
        return;
    }

    Hit hit = server.hit_test(cursor->x, cursor->y);

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
            wlr_seat_pointer_notify_clear_focus(wlr);
            wlr_cursor_set_xcursor(cursor, xcursor, wlr_xcursor_get_resize_name(wlr_edges(zone.edges)));
            return;
        }
        if (hit.backdrop) {
            set_titlebar_hover(nullptr, 0);
            wlr_seat_pointer_notify_clear_focus(wlr);
            set_default_cursor();
            return;
        }
        if (hit.titlebar) {
            set_titlebar_hover(hit.titlebar, int(hit.titlebar->part_at(hit.sx, hit.sy)));
            wlr_seat_pointer_notify_clear_focus(wlr);
            set_default_cursor();
            return;
        }
        set_titlebar_hover(nullptr, 0);
    }

    // Implicit grab: while a button is held, events stay with the surface
    // that got the press, even when the cursor leaves it.
    if (mode == Mode::Pressed && !wlr->drag && focused && hit.surface != focused) {
        if (Owner owner = Server::owner_of(focused)) {
            double ox, oy;
            if (owner.layer) {
                ox = owner.layer->tree->node.x;
                oy = owner.layer->tree->node.y;
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

    if (!hit.surface && !wlr->drag)
        set_default_cursor();
    pointer_focus(hit.view, hit.surface, hit.sx, hit.sy, time);
}

void Seat::pointer_focus(View*, wlr_surface* surface, double sx, double sy, uint32_t time) {
    if (!surface) {
        wlr_seat_pointer_notify_clear_focus(wlr);
        return;
    }
    if (!time)
        time = now_ms();
    // Enter is a no-op when the surface already has pointer focus.
    wlr_seat_pointer_notify_enter(wlr, surface, sx, sy);
    wlr_seat_pointer_notify_motion(wlr, time, sx, sy);
}

void Seat::button(wlr_pointer_button_event* e) {
    wlr_idle_notifier_v1_notify_activity(server.idle_notifier, wlr);

    // A press on the overview is the overview's, and so is its release, even
    // when the overview has gone by then.
    const bool pressed = e->state == WL_POINTER_BUTTON_STATE_PRESSED;
    if (server.overview->active() || (!pressed && overview_press_)) {
        overview_press_ = pressed;
        server.overview->button(cursor->x, cursor->y, e->button, pressed);
        return;
    }

    if (e->state == WL_POINTER_BUTTON_STATE_PRESSED) {
        mode = Mode::Pressed;
        if (Output* o = server.output_at(cursor->x, cursor->y))
            server.focused_output = o;
        if (server.locked) {
            wlr_seat_pointer_notify_button(wlr, e->time_msec, e->button, e->state);
            return;
        }

        Hit hit = server.hit_test(cursor->x, cursor->y);

        // Frame edges and title bars belong to atrium, not the client.
        if (ResizeZone zone = resize_zone(cursor->x, cursor->y, hit); zone.view) {
            server.focus_view(zone.view);
            if (e->button == BTN_LEFT)
                begin_resize(zone.view, zone.edges);
            return;
        }
        if (hit.titlebar && titlebar_button(e, hit))
            return;
        // Clicking the dimmed screen puts a secret space away.
        if (hit.backdrop) {
            server.hide_secret();
            return;
        }

        // Click to focus, and a click raises: the desktop model, not the tiling one.
        if (hit.view && (!hit.view->unmanaged() || hit.view->wants_focus()))
            server.focus_view(hit.view);
        else if (hit.layer && hit.layer->wlr->current.keyboard_interactive)
            server.focus_layer(hit.layer);

        // Mod + drag moves (left) or resizes (right) from anywhere in a window.
        wlr_keyboard* kb = wlr_seat_get_keyboard(wlr);
        uint32_t mods = kb ? wlr_keyboard_get_modifiers(kb) : 0;
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
            titlebar_button(e, server.hit_test(cursor->x, cursor->y));
            return;
        }
        if (!server.locked && (mode == Mode::Move || mode == Mode::Resize)) {
            // The grab ate the press; the release ends it and is ours too.
            // Dropped over a snap zone, the window takes it.
            View* dropped = mode == Mode::Move ? grab_view_ : nullptr;
            const uint32_t zone = snap_zone_;
            cancel_grab();
            if (dropped && zone)
                dropped->snap(zone);
            wlr_seat_pointer_clear_focus(wlr);
            set_default_cursor();
            refresh_pointer();
            return;
        }
        mode = Mode::Normal;
    }
    wlr_seat_pointer_notify_button(wlr, e->time_msec, e->button, e->state);
}

// --- title bars and frame edges -----------------------------------------------------

Seat::ResizeZone Seat::resize_zone(double lx, double ly, const Hit& hit) const {
    constexpr int kBand = 8;    // resize band just outside the frame
    constexpr int kCorner = 16; // along an edge, this close to a corner resizes both ways
    if (server.locked)
        return {};
    // Panels, docks and menus above windows keep their clicks.
    if (hit.layer && hit.layer->wlr->current.layer >= ZWLR_LAYER_SHELL_V1_LAYER_TOP)
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
        if (!v->titlebar || v->fullscreen || v->maximized)
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
bool Seat::titlebar_button(wlr_pointer_button_event* e, const Hit& hit) {
    using Part = Titlebar::Part;
    if (e->state == WL_POINTER_BUTTON_STATE_PRESSED) {
        if (e->button != BTN_LEFT) {
            server.focus_view(hit.view);
            return true;
        }
        const Part part = hit.titlebar->part_at(hit.sx, hit.sy);
        if (part == Part::Bar) {
            server.focus_view(hit.view);
            // Double-click zooms, like macOS.
            const bool twice = last_bar_click_view_ == hit.view && e->time_msec - last_bar_click_ms_ < 400;
            last_bar_click_view_ = twice ? nullptr : hit.view;
            last_bar_click_ms_ = e->time_msec;
            if (twice && !hit.view->fullscreen)
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
        wlr_seat_pointer_clear_focus(wlr);
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
    case Part::Minimize: v.set_minimized(true); break;
    case Part::Maximize: if (!v.fullscreen) v.set_maximized(!v.maximized); break;
    default: break;
    }
    refresh_pointer();
    return true;
}

void Seat::axis(wlr_pointer_axis_event* e) {
    wlr_idle_notifier_v1_notify_activity(server.idle_notifier, wlr);
    wlr_seat_pointer_notify_axis(wlr, e->time_msec, e->orientation, e->delta, e->delta_discrete,
                                 e->source, e->relative_direction);
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
    grab_unmaximize_ = view->maximized || view->snapped;
    snap_zone_ = 0;
    mode = Mode::Move;
    wlr_seat_pointer_clear_focus(wlr);
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
    if (mode == Mode::Move || mode == Mode::Resize || view->fullscreen || view->unmanaged() || !edges)
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
    wlr_seat_pointer_clear_focus(wlr);
    wlr_cursor_set_xcursor(cursor, xcursor, wlr_xcursor_get_resize_name(wlr_edges(edges)));
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

void Seat::new_constraint(wlr_pointer_constraint_v1* c) {
    auto constraint = std::make_unique<Constraint>(c);
    Constraint* raw = constraint.get();
    raw->destroy.connect(&c->events.destroy, [this, raw](void*) {
        if (active_constraint_ == raw->wlr) {
            warp_to_constraint_hint();
            active_constraint_ = nullptr;
        }
        std::erase_if(constraints_, [raw](auto& p) { return p.get() == raw; });
    });
    constraints_.push_back(std::move(constraint));
}

void Seat::activate_constraint(wlr_pointer_constraint_v1* c) {
    if (active_constraint_ == c)
        return;
    if (active_constraint_)
        wlr_pointer_constraint_v1_send_deactivated(active_constraint_);
    active_constraint_ = c;
    if (c)
        wlr_pointer_constraint_v1_send_activated(c);
}

void Seat::warp_to_constraint_hint() {
    auto* c = active_constraint_;
    if (!c->current.cursor_hint.enabled)
        return;
    Owner owner = Server::owner_of(c->surface);
    if (!owner.view)
        return;
    double ox, oy;
    owner.view->surface_origin(ox, oy);
    const double sx = c->current.cursor_hint.x, sy = c->current.cursor_hint.y;
    wlr_cursor_warp(cursor, nullptr, ox + sx, oy + sy);
    wlr_seat_pointer_warp(c->seat, sx, sy);
}

} // namespace atrium
