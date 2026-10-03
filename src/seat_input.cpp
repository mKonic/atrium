// The seat's side of libinput: devices coming and going, and what they send
// (keys, pointer, gestures, touch, tablets, switches).
#include "seat.hpp"

#include "ipc.hpp"
#include "layer_surface.hpp"
#include "output.hpp"
#include "overview.hpp"
#include "server.hpp"
#include "view.hpp"

#include <algorithm>
#include <cmath>

namespace atrium {

namespace {

// How far three fingers travel (libinput's pointer units) to switch a space
// or open the overview.
constexpr double kSwipeDistance = 120;

bool internal_panel(const std::string& name) {
    return name.starts_with("eDP") || name.starts_with("LVDS") || name.starts_with("DSI");
}

} // namespace

// ---- devices -----------------------------------------------------------------------

void Seat::device_added(input::Device& d) {
    if (d.keyboard)
        ++libinput_keyboards_;
    if (d.pointer)
        configure_libinput(d.handle);
    if (d.tablet) {
        wl::Tablets::TabletInfo info;
        info.name = d.name;
        info.path = libinput_device_get_sysname(d.handle);
        info.vid = libinput_device_get_id_vendor(d.handle);
        info.pid = libinput_device_get_id_product(d.handle);
        d.data = server.wl->tablets->add_tablet(info);
    } else if (d.pad) {
        wl::Tablets::PadInfo info;
        info.path = libinput_device_get_sysname(d.handle);
        info.buttons = uint32_t(std::max(0, libinput_device_tablet_pad_get_num_buttons(d.handle)));
        const int groups = libinput_device_tablet_pad_get_num_mode_groups(d.handle);
        for (int g = 0; g < groups; ++g) {
            libinput_tablet_pad_mode_group* mg = libinput_device_tablet_pad_get_mode_group(d.handle, unsigned(g));
            wl::Tablets::PadGroup pg;
            for (uint32_t b = 0; b < info.buttons; ++b)
                if (libinput_tablet_pad_mode_group_has_button(mg, b))
                    pg.buttons.push_back(b);
            for (int r = 0; r < libinput_device_tablet_pad_get_num_rings(d.handle); ++r)
                pg.rings += libinput_tablet_pad_mode_group_has_ring(mg, unsigned(r));
            for (int st = 0; st < libinput_device_tablet_pad_get_num_strips(d.handle); ++st)
                pg.strips += libinput_tablet_pad_mode_group_has_strip(mg, unsigned(st));
            pg.modes = libinput_tablet_pad_mode_group_get_num_modes(mg);
            info.groups.push_back(std::move(pg));
        }
        d.data = server.wl->tablets->add_pad(info);
    }
    update_capabilities();
}

void Seat::device_removed(input::Device& d) {
    if (d.keyboard && libinput_keyboards_ > 0)
        --libinput_keyboards_;
    if (d.tablet && d.data)
        server.wl->tablets->remove(static_cast<wl::Tablets::Tablet*>(d.data));
    else if (d.pad && d.data)
        server.wl->tablets->remove(static_cast<wl::Tablets::Pad*>(d.data));
    d.data = nullptr;
    update_capabilities();
}

// ---- keys and pointer ----------------------------------------------------------------

void Seat::key(input::Device&, uint32_t time_ms, uint32_t keycode, bool pressed) {
    keyboards_->keys.key(time_ms, keycode, pressed);
}

void Seat::motion(input::Device&, uint32_t time_ms, double dx, double dy, double dx_unaccel, double dy_unaccel) {
    motion(time_ms, dx, dy, dx_unaccel, dy_unaccel);
}

void Seat::motion_absolute(input::Device& d, uint32_t time_ms, double x, double y) {
    double lx, ly;
    to_layout(d, x, y, &lx, &ly);
    motion_absolute(time_ms, lx, ly);
}

void Seat::button(input::Device&, uint32_t time_ms, uint32_t b, bool pressed) {
    button(ButtonEvent{time_ms, b, pressed});
}

void Seat::scroll(input::Device& d, const input::Scroll& s) {
    const bool natural = libinput_device_config_scroll_get_natural_scroll_enabled(d.handle);
    axis(AxisEvent{s.time_ms, s.orientation, s.delta, s.value120, uint32_t(s.source), natural});
}

void Seat::frame(input::Device&) {
    server.wl->seat->pointer_frame();
}

void Seat::to_layout(const input::Device& d, double x, double y, double* lx, double* ly) const {
    Box b = server.layout_box;
    const std::string name = d.output_name();
    for (const Output* o : server.outputs)
        if (o->enabled() && (name.empty() ? server.outputs.size() == 1 : name == o->screen->name))
            b = o->box;
    // A touchscreen udev doesn't name, with several screens: the built-in one.
    if (name.empty() && server.outputs.size() > 1 && (d.touch || d.tablet))
        for (const Output* o : server.outputs)
            if (o->enabled() && internal_panel(o->screen->name))
                b = o->box;
    *lx = b.x + x * b.width;
    *ly = b.y + y * b.height;
}

Seat::SurfaceAt Seat::surface_at(double lx, double ly) const {
    const Hit hit = server.hit_test(lx, ly);
    return {hit.surface, hit.sx, hit.sy};
}

// ---- gestures ----------------------------------------------------------------------

// Three or four fingers are atrium's, as on a Mac: sideways to the space
// beside, up for Mission Control, down for the app's windows (or back).
// Pinches and holds are the app's (a browser's zoom).
void Seat::swipe(input::Device&, libinput_event_gesture* e, libinput_event_type type) {
    const uint32_t time = uint32_t(libinput_event_gesture_get_time(e));
    if (type == LIBINPUT_EVENT_GESTURE_SWIPE_BEGIN) {
        const int fingers = libinput_event_gesture_get_finger_count(e);
        swipe_ = {uint32_t(fingers), 0, 0, fingers >= 3 && !server.locked};
        if (!swipe_.ours)
            server.wl->pointer_gestures->swipe_begin(time, uint32_t(fingers));
        return;
    }
    if (type == LIBINPUT_EVENT_GESTURE_SWIPE_UPDATE) {
        const double dx = libinput_event_gesture_get_dx(e), dy = libinput_event_gesture_get_dy(e);
        swipe_.dx += dx;
        swipe_.dy += dy;
        if (!swipe_.ours)
            server.wl->pointer_gestures->swipe_update(time, dx, dy);
        return;
    }
    const bool cancelled = libinput_event_gesture_get_cancelled(e);
    if (!swipe_.ours) {
        server.wl->pointer_gestures->swipe_end(time, cancelled);
        return;
    }
    swipe_.ours = false;
    if (cancelled)
        return;
    server.note_activity();
    const double dx = swipe_.dx, dy = swipe_.dy;
    if (std::abs(dx) >= std::abs(dy) && std::abs(dx) >= kSwipeDistance) {
        // The content follows the fingers: a swipe left shows the space on the right.
        server.step_space(dx < 0 ? 1 : -1);
    } else if (std::abs(dy) > std::abs(dx) && std::abs(dy) >= kSwipeDistance) {
        const bool open = server.overview->active();
        if (dy < 0 && !open)
            server.run_action({.mods = 0, .sym = 0, .action = Action::Overview});
        else if (dy > 0 && open)
            server.run_action({.mods = 0, .sym = 0, .action = Action::Overview});  // closes it
        else if (dy > 0)
            server.run_action({.mods = 0, .sym = 0, .action = Action::AppExpose});
    }
}

void Seat::pinch(input::Device&, libinput_event_gesture* e, libinput_event_type type) {
    const uint32_t time = uint32_t(libinput_event_gesture_get_time(e));
    auto& g = *server.wl->pointer_gestures;
    if (type == LIBINPUT_EVENT_GESTURE_PINCH_BEGIN)
        g.pinch_begin(time, uint32_t(libinput_event_gesture_get_finger_count(e)));
    else if (type == LIBINPUT_EVENT_GESTURE_PINCH_UPDATE)
        g.pinch_update(time, libinput_event_gesture_get_dx(e), libinput_event_gesture_get_dy(e),
                       libinput_event_gesture_get_scale(e), libinput_event_gesture_get_angle_delta(e));
    else
        g.pinch_end(time, libinput_event_gesture_get_cancelled(e));
}

void Seat::hold(input::Device&, libinput_event_gesture* e, libinput_event_type type) {
    const uint32_t time = uint32_t(libinput_event_gesture_get_time(e));
    auto& g = *server.wl->pointer_gestures;
    if (type == LIBINPUT_EVENT_GESTURE_HOLD_BEGIN)
        g.hold_begin(time, uint32_t(libinput_event_gesture_get_finger_count(e)));
    else
        g.hold_end(time, libinput_event_gesture_get_cancelled(e));
}

// ---- touch -----------------------------------------------------------------------------

void Seat::touch(input::Device& d, libinput_event_touch* e, libinput_event_type type) {
    const uint32_t time = libinput_event_touch_get_time(e);
    double lx = 0, ly = 0;
    if (type == LIBINPUT_EVENT_TOUCH_DOWN || type == LIBINPUT_EVENT_TOUCH_MOTION)
        to_layout(d, libinput_event_touch_get_x_transformed(e, 1), libinput_event_touch_get_y_transformed(e, 1), &lx,
                  &ly);
    switch (type) {
    case LIBINPUT_EVENT_TOUCH_DOWN:
        touch_down(time, libinput_event_touch_get_seat_slot(e), lx, ly);
        break;
    case LIBINPUT_EVENT_TOUCH_MOTION:
        touch_motion(time, libinput_event_touch_get_seat_slot(e), lx, ly);
        break;
    case LIBINPUT_EVENT_TOUCH_UP:
        touch_up(time, libinput_event_touch_get_seat_slot(e));
        break;
    case LIBINPUT_EVENT_TOUCH_CANCEL:
        touch_cancel();
        break;
    case LIBINPUT_EVENT_TOUCH_FRAME:
        server.wl->seat->touch_frame();
        break;
    default:
        break;
    }
}

void Seat::touch_down(uint32_t time, int32_t id, double lx, double ly) {
    server.note_activity();
    const Hit hit = server.hit_test(lx, ly);
    // A touch focuses what it lands on, as a click does.
    if (!server.locked && hit.view && (!hit.view->unmanaged() || hit.view->wants_focus()))
        server.focus_view(hit.view);
    if (!hit.surface)
        return;
    touches_[id] = hit.surface;
    server.wl->seat->touch_down(time, hit.surface, id, hit.sx, hit.sy);
}

void Seat::touch_motion(uint32_t time, int32_t id, double lx, double ly) {
    server.note_activity();
    auto it = touches_.find(id);
    if (it == touches_.end())
        return;
    // In the surface the touch began on, wherever it goes now.
    const Owner owner = Server::owner_of(it->second);
    double ox = 0, oy = 0;
    if (owner.view)
        owner.view->surface_origin(ox, oy);
    else if (owner.layer)
        ox = owner.layer->tree->x, oy = owner.layer->tree->y;
    server.wl->seat->touch_motion(time, id, lx - ox, ly - oy);
}

void Seat::touch_up(uint32_t time, int32_t id) {
    server.note_activity();
    if (touches_.erase(id))
        server.wl->seat->touch_up(time, id);
}

void Seat::touch_cancel() {
    touches_.clear();
    server.wl->seat->touch_cancel();
}

// ---- tablets -----------------------------------------------------------------------------

namespace {

uint32_t tool_type(libinput_tablet_tool_type t) {
    switch (t) {
    case LIBINPUT_TABLET_TOOL_TYPE_ERASER: return 0x141;
    case LIBINPUT_TABLET_TOOL_TYPE_BRUSH: return 0x142;
    case LIBINPUT_TABLET_TOOL_TYPE_PENCIL: return 0x143;
    case LIBINPUT_TABLET_TOOL_TYPE_AIRBRUSH: return 0x144;
    case LIBINPUT_TABLET_TOOL_TYPE_MOUSE: return 0x146;
    case LIBINPUT_TABLET_TOOL_TYPE_LENS: return 0x147;
    default: return 0x140;  // pen
    }
}

} // namespace

// A tool the app under it takes gets tablet events; elsewhere it moves the
// pointer, its tip a left click.
void Seat::tablet_tool(input::Device& d, libinput_event_tablet_tool* e, libinput_event_type type) {
    auto* tablet = static_cast<wl::Tablets::Tablet*>(d.data);
    if (!tablet)
        return;
    wl::Tablets& tablets = *server.wl->tablets;
    libinput_tablet_tool* lt = libinput_event_tablet_tool_get_tool(e);
    wl::Tablets::Tool*& tool = tools_[lt];
    if (!tool) {
        wl::Tablets::ToolInfo info;
        info.type = tool_type(libinput_tablet_tool_get_type(lt));
        info.hardware_serial = libinput_tablet_tool_get_serial(lt);
        info.hardware_id_wacom = libinput_tablet_tool_get_tool_id(lt);
        if (libinput_tablet_tool_has_tilt(lt))
            info.capabilities.push_back(1);
        if (libinput_tablet_tool_has_pressure(lt))
            info.capabilities.push_back(2);
        if (libinput_tablet_tool_has_distance(lt))
            info.capabilities.push_back(3);
        if (libinput_tablet_tool_has_rotation(lt))
            info.capabilities.push_back(4);
        if (libinput_tablet_tool_has_slider(lt))
            info.capabilities.push_back(5);
        if (libinput_tablet_tool_has_wheel(lt))
            info.capabilities.push_back(6);
        tool = tablets.add_tool(info);
    }
    const uint32_t time = uint32_t(libinput_event_tablet_tool_get_time(e));
    server.note_activity();
    double lx, ly;
    to_layout(d, libinput_event_tablet_tool_get_x_transformed(e, 1), libinput_event_tablet_tool_get_y_transformed(e, 1),
              &lx, &ly);
    const SurfaceAt at = surface_at(lx, ly);
    const bool app = at.surface && tablets.bound_by(tool, at.surface->client());

    if (type == LIBINPUT_EVENT_TABLET_TOOL_PROXIMITY &&
        libinput_event_tablet_tool_get_proximity_state(e) == LIBINPUT_TABLET_TOOL_PROXIMITY_STATE_OUT) {
        tablets.proximity_out(tool);
        return;
    }
    if (!app) {
        // Not the app's: the pointer moves, the tip clicks.
        if (tablets.focus(tool))
            tablets.proximity_out(tool);
        motion_absolute(time, lx, ly);
        if (type == LIBINPUT_EVENT_TABLET_TOOL_TIP)
            button(ButtonEvent{time, BTN_LEFT,
                               libinput_event_tablet_tool_get_tip_state(e) == LIBINPUT_TABLET_TOOL_TIP_DOWN});
        else if (type == LIBINPUT_EVENT_TABLET_TOOL_BUTTON)
            button(ButtonEvent{time, libinput_event_tablet_tool_get_button(e),
                               libinput_event_tablet_tool_get_button_state(e) == LIBINPUT_BUTTON_STATE_PRESSED});
        server.wl->seat->pointer_frame();
        return;
    }
    cursor->warp_closest(lx, ly);
    if (tablets.focus(tool) != at.surface)
        tablets.proximity_in(tool, tablet, at.surface, at.sx, at.sy);
    else
        tablets.motion(tool, at.sx, at.sy);
    if (libinput_event_tablet_tool_pressure_has_changed(e))
        tablets.pressure(tool, libinput_event_tablet_tool_get_pressure(e));
    if (libinput_event_tablet_tool_distance_has_changed(e))
        tablets.distance(tool, libinput_event_tablet_tool_get_distance(e));
    if (libinput_event_tablet_tool_tilt_x_has_changed(e) || libinput_event_tablet_tool_tilt_y_has_changed(e))
        tablets.tilt(tool, libinput_event_tablet_tool_get_tilt_x(e), libinput_event_tablet_tool_get_tilt_y(e));
    if (libinput_event_tablet_tool_rotation_has_changed(e))
        tablets.rotation(tool, libinput_event_tablet_tool_get_rotation(e));
    if (libinput_event_tablet_tool_slider_has_changed(e))
        tablets.slider(tool, libinput_event_tablet_tool_get_slider_position(e));
    if (libinput_event_tablet_tool_wheel_has_changed(e))
        tablets.wheel(tool, libinput_event_tablet_tool_get_wheel_delta(e),
                      int32_t(libinput_event_tablet_tool_get_wheel_delta_discrete(e)));
    if (type == LIBINPUT_EVENT_TABLET_TOOL_TIP) {
        if (libinput_event_tablet_tool_get_tip_state(e) == LIBINPUT_TABLET_TOOL_TIP_DOWN) {
            // Touching a window focuses it, as a click does.
            if (View* v = Server::owner_of(at.surface).view; v && v != server.focused_view)
                server.focus_view(v);
            tablets.down(tool);
        } else {
            tablets.up(tool);
        }
    } else if (type == LIBINPUT_EVENT_TABLET_TOOL_BUTTON) {
        tablets.button(tool, libinput_event_tablet_tool_get_button(e),
                       libinput_event_tablet_tool_get_button_state(e) == LIBINPUT_BUTTON_STATE_PRESSED);
    }
    tablets.frame(tool, time);
}

// A pad's buttons, rings and strips go to the window with the keyboard.
void Seat::tablet_pad(input::Device& d, libinput_event_tablet_pad* e, libinput_event_type type) {
    auto* pad = static_cast<wl::Tablets::Pad*>(d.data);
    if (!pad)
        return;
    wl::Tablets& tablets = *server.wl->tablets;
    wl::Surface* focus = server.wl->seat->keyboard_focus();
    if (tablets.focus(pad) != focus) {
        tablets.pad_leave(pad);
        // The pad belongs with a tablet of the same device group: the first.
        wl::Tablets::Tablet* tablet = nullptr;
        for (const auto& dev : libinput_->devices())
            if (dev->tablet && dev->data &&
                libinput_device_get_device_group(dev->handle) == libinput_device_get_device_group(d.handle))
                tablet = static_cast<wl::Tablets::Tablet*>(dev->data);
        if (focus && tablet)
            tablets.pad_enter(pad, tablet, focus);
    }
    if (!tablets.focus(pad))
        return;
    const uint32_t time = uint32_t(libinput_event_tablet_pad_get_time(e));
    switch (type) {
    case LIBINPUT_EVENT_TABLET_PAD_BUTTON:
        tablets.pad_button(pad, time, libinput_event_tablet_pad_get_button_number(e),
                           libinput_event_tablet_pad_get_button_state(e) == LIBINPUT_BUTTON_STATE_PRESSED);
        break;
    case LIBINPUT_EVENT_TABLET_PAD_RING: {
        const double pos = libinput_event_tablet_pad_get_ring_position(e);
        tablets.pad_ring(pad, libinput_event_tablet_pad_get_ring_number(e), pos < 0 ? std::nullopt : std::optional(pos),
                         libinput_event_tablet_pad_get_ring_source(e) == LIBINPUT_TABLET_PAD_RING_SOURCE_FINGER, time);
        break;
    }
    case LIBINPUT_EVENT_TABLET_PAD_STRIP: {
        const double pos = libinput_event_tablet_pad_get_strip_position(e);
        tablets.pad_strip(pad, libinput_event_tablet_pad_get_strip_number(e),
                          pos < 0 ? std::nullopt : std::optional(pos),
                          libinput_event_tablet_pad_get_strip_source(e) == LIBINPUT_TABLET_PAD_STRIP_SOURCE_FINGER,
                          time);
        break;
    }
    default:
        break;
    }
}

// ---- switches ------------------------------------------------------------------------------

bool Seat::lid_keeps_off(const Output* o) const {
    return lid_closed_ && internal_panel(o->screen->name);
}

// The lid closed with another screen on: the built-in one goes dark (and
// comes back when the lid opens). Tablet mode is the shell's to show.
void Seat::toggle(input::Device&, libinput_switch which, bool on) {
    if (which == LIBINPUT_SWITCH_TABLET_MODE) {
        if (server.ipc)
            server.ipc->broadcast("outputs", {{"event", "input.tablet_mode"}, {"on", on}});
        return;
    }
    if (which != LIBINPUT_SWITCH_LID || on == lid_closed_)
        return;
    lid_closed_ = on;
    const bool other = std::ranges::any_of(server.outputs, [](const Output* o) {
        return o->enabled() && !internal_panel(o->screen->name);
    });
    for (Output* o : server.outputs) {
        if (!internal_panel(o->screen->name) || (on && !other))
            continue;
        backend::OutputState state;
        state.set_enabled(!on);
        o->screen->commit_state(state);
        o->asleep = on;
    }
    server.update_outputs();
}

} // namespace atrium
