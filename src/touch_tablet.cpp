// Touchscreens, drawing tablets and their pads, as labwc handles them
// (src/input/touch.c, tablet.c, tablet-pad.c): what lands on an app that
// speaks touch or the tablet protocol goes to it as such; anywhere else (the
// title bar, an app without them, the overview) it works the pointer, a touch
// or the pen's tip being a left click.
#include "touch_tablet.hpp"

#include "seat.hpp"

#include "layer_surface.hpp"
#include "overview.hpp"
#include "output.hpp"
#include "server.hpp"
#include "view.hpp"

#include <algorithm>

namespace atrium {

namespace {

// Pen buttons to mouse buttons: labwc's default (the lower one is right).
uint32_t mouse_button_for(uint32_t pen_button) {
    switch (pen_button) {
    case BTN_STYLUS: return BTN_RIGHT;
    case BTN_STYLUS2: return BTN_MIDDLE;
    case BTN_STYLUS3: return BTN_SIDE;
    default: return 0;
    }
}

libinput_device_group* group_of(wlr_input_device* device) {
    if (!wlr_input_device_is_libinput(device))
        return nullptr;
    return libinput_device_get_device_group(wlr_libinput_get_device_handle(device));
}

} // namespace

// --- devices ----------------------------------------------------------------

void Seat::map_to_outputs() {
    for (auto& d : mapped_) {
        wlr_output* to = nullptr;
        for (Output* o : server.outputs)
            if (o->enabled() && o->wlr->name == d->output)
                to = o->wlr;
        wlr_cursor_map_input_to_output(cursor, d->device, to);
    }
}

void Seat::add_touch(wlr_touch* touch) {
    wlr_cursor_attach_input_device(cursor, &touch->base);
    auto d = std::make_unique<InputDevice>();
    d->device = &touch->base;
    d->output = touch->output_name ? touch->output_name : "";
    InputDevice* raw = d.get();
    d->destroy.connect(&touch->base.events.destroy, [this, raw](void*) {
        std::erase_if(mapped_, [raw](const auto& m) { return m.get() == raw; });
        update_capabilities();
    });
    mapped_.push_back(std::move(d));
    map_to_outputs();
    if (!touch_down_.connected()) {
        touch_down_.connect(&cursor->events.touch_down, [this](wlr_touch_down_event* e) { touch_down(e); });
        touch_up_.connect(&cursor->events.touch_up, [this](wlr_touch_up_event* e) { touch_up(e); });
        touch_motion_.connect(&cursor->events.touch_motion, [this](wlr_touch_motion_event* e) { touch_motion(e); });
        touch_frame_.connect(&cursor->events.touch_frame, [this](void*) { wlr_seat_touch_notify_frame(wlr); });
    }
}

void Seat::add_tablet(wlr_tablet* tablet) {
    wlr_cursor_attach_input_device(cursor, &tablet->base);
    auto d = std::make_unique<InputDevice>();
    d->device = &tablet->base;
    // Tablets don't say which screen they belong to: all of them, as wlroots
    // maps them by default.
    InputDevice* raw = d.get();
    d->destroy.connect(&tablet->base.events.destroy, [this, raw](void*) {
        std::erase_if(mapped_, [raw](const auto& m) { return m.get() == raw; });
    });
    mapped_.push_back(std::move(d));
    map_to_outputs();

    auto t = std::make_unique<TabletDevice>();
    t->tablet = tablet;
    t->v2 = wlr_tablet_create(server.tablet_manager, wlr, &tablet->base);
    tablet->data = t.get();
    TabletDevice* rt = t.get();
    t->destroy.connect(&tablet->base.events.destroy, [this, rt](void*) {
        for (auto& p : pads_)
            if (p->tablet == rt)
                p->tablet = nullptr;
        std::erase_if(tablets_, [rt](const auto& x) { return x.get() == rt; });
    });
    // A pad already here may belong to it.
    for (auto& p : pads_)
        if (!p->tablet && group_of(&p->pad->base) && group_of(&p->pad->base) == group_of(&tablet->base))
            p->tablet = rt;
    tablets_.push_back(std::move(t));
    wlr_log(WLR_INFO, "tablet: %s, %.0f x %.0f mm", tablet->base.name ? tablet->base.name : "?", tablet->width_mm,
            tablet->height_mm);

    if (!tool_proximity_.connected()) {
        tool_proximity_.connect(&cursor->events.tablet_tool_proximity,
                                [this](wlr_tablet_tool_proximity_event* e) { tool_proximity(e); });
        tool_axis_.connect(&cursor->events.tablet_tool_axis, [this](wlr_tablet_tool_axis_event* e) { tool_axis(e); });
        tool_tip_.connect(&cursor->events.tablet_tool_tip, [this](wlr_tablet_tool_tip_event* e) { tool_tip(e); });
        tool_button_.connect(&cursor->events.tablet_tool_button,
                             [this](wlr_tablet_tool_button_event* e) { tool_button(e); });
    }
}

void Seat::add_tablet_pad(wlr_tablet_pad* pad) {
    auto p = std::make_unique<TabletPad>();
    p->pad = pad;
    p->v2 = wlr_tablet_pad_create(server.tablet_manager, wlr, &pad->base);
    for (auto& t : tablets_)
        if (group_of(&pad->base) && group_of(&pad->base) == group_of(&t->tablet->base))
            p->tablet = t.get();
    TabletPad* rp = p.get();
    // To the app with the keyboard; with none that takes it, as mouse buttons.
    p->button.connect(&pad->events.button, [this, rp](wlr_tablet_pad_button_event* e) {
        const bool pressed = e->state == WLR_BUTTON_PRESSED;
        if (rp->v2 && rp->surface && rp->tablet) {
            wlr_tablet_v2_tablet_pad_notify_button(rp->v2, e->button, e->time_msec,
                pressed ? ZWP_TABLET_PAD_V2_BUTTON_STATE_PRESSED : ZWP_TABLET_PAD_V2_BUTTON_STATE_RELEASED);
        } else if (uint32_t b = mouse_button_for(e->button)) {
            emulate_button(b, pressed, e->time_msec);
        }
    });
    p->ring.connect(&pad->events.ring, [rp](wlr_tablet_pad_ring_event* e) {
        if (rp->v2 && rp->surface && rp->tablet)
            wlr_tablet_v2_tablet_pad_notify_ring(rp->v2, e->ring, e->position,
                                                 e->source == WLR_TABLET_PAD_RING_SOURCE_FINGER, e->time_msec);
    });
    p->strip.connect(&pad->events.strip, [rp](wlr_tablet_pad_strip_event* e) {
        if (rp->v2 && rp->surface && rp->tablet)
            wlr_tablet_v2_tablet_pad_notify_strip(rp->v2, e->strip, e->position,
                                                  e->source == WLR_TABLET_PAD_STRIP_SOURCE_FINGER, e->time_msec);
    });
    p->destroy.connect(&pad->base.events.destroy, [this, rp](void*) {
        std::erase_if(pads_, [rp](const auto& x) { return x.get() == rp; });
    });
    pads_.push_back(std::move(p));
    if (wlr_surface* s = wlr->keyboard_state.focused_surface)
        pads_enter(s);
}

void Seat::pads_enter(wlr_surface* surface) {
    for (auto& p : pads_) {
        if (!p->v2 || !p->tablet || !p->tablet->v2 || p->surface == surface)
            continue;
        if (p->surface)
            wlr_tablet_v2_tablet_pad_notify_leave(p->v2, p->surface);
        p->surface_destroy.disconnect();
        p->surface = surface;
        if (!surface)
            continue;
        wlr_tablet_v2_tablet_pad_notify_enter(p->v2, p->tablet->v2, surface);
        TabletPad* rp = p.get();
        p->surface_destroy.connect(&surface->events.destroy, [rp](void*) {
            rp->surface = nullptr;
            rp->surface_destroy.disconnect();
        });
    }
}

// --- shared -----------------------------------------------------------------

wlr_surface* Seat::touch_target(double lx, double ly, double& ox, double& oy, bool tablet) {
    ox = oy = 0;
    // The overview and the switcher take the pointer; so do they a touch.
    if (server.overview->active() || mode != Mode::Normal)
        return nullptr;
    const Hit hit = server.hit_test(lx, ly);
    if (!hit.surface)
        return nullptr;
    if (tablet ? !wlr_surface_accepts_tablet_v2(hit.surface, tablets_.empty() ? nullptr : tablets_.front()->v2)
               : !wlr_surface_accepts_touch(hit.surface, wlr))
        return nullptr;
    ox = lx - hit.sx;
    oy = ly - hit.sy;
    return hit.surface;
}

// A touch or the pen's tip on a window chooses it, as a click does.
void Seat::focus_for_touch(const Hit& hit) {
    if (hit.view && !hit.view->unmanaged() && hit.view != server.focused_view)
        server.focus_view(hit.view);
    else if (hit.layer && hit.layer->wlr->current.keyboard_interactive)
        server.focus_layer(hit.layer);
}

void Seat::emulate_absolute(wlr_input_device* device, double x, double y, uint32_t time) {
    double lx, ly;
    wlr_cursor_absolute_to_layout_coords(cursor, device, x, y, &lx, &ly);
    motion(time ? time : 1, device, lx - cursor->x, ly - cursor->y, lx - cursor->x, ly - cursor->y);
    wlr_seat_pointer_notify_frame(wlr);
}

void Seat::emulate_button(uint32_t button_code, bool pressed, uint32_t time) {
    wlr_pointer_button_event e{};
    e.time_msec = time;
    e.button = button_code;
    e.state = pressed ? WL_POINTER_BUTTON_STATE_PRESSED : WL_POINTER_BUTTON_STATE_RELEASED;
    button(&e);
    wlr_seat_pointer_notify_frame(wlr);
}

// --- touch ------------------------------------------------------------------

void Seat::touch_down(wlr_touch_down_event* e) {
    server.note_activity();
    double lx, ly;
    wlr_cursor_absolute_to_layout_coords(cursor, &e->touch->base, e->x, e->y, &lx, &ly);
    TouchPoint p{e->touch_id, nullptr, 0, 0};
    p.surface = touch_target(lx, ly, p.ox, p.oy, false);
    touch_points_.push_back(p);
    const bool first = touch_points_.size() == 1;

    // The pointer goes out of the way while fingers do the work.
    if (!typing_hidden_) {
        typing_hidden_ = true;
        wlr_cursor_unset_image(cursor);
    }
    if (p.surface) {
        focus_for_touch(server.hit_test(lx, ly));
        wlr_seat_pointer_notify_clear_focus(wlr);
        if (first)
            wlr_cursor_warp_absolute(cursor, &e->touch->base, e->x, e->y);
        wlr_seat_touch_notify_down(wlr, p.surface, e->time_msec, e->touch_id, lx - p.ox, ly - p.oy);
        return;
    }
    if (first)
        emulate_absolute(&e->touch->base, e->x, e->y, e->time_msec);
    emulate_button(BTN_LEFT, true, e->time_msec);
}

void Seat::touch_motion(wlr_touch_motion_event* e) {
    server.note_activity();
    auto it = std::ranges::find(touch_points_, e->touch_id, &TouchPoint::id);
    if (it == touch_points_.end())
        return;
    const bool only = touch_points_.size() == 1;
    if (it->surface) {
        double lx, ly;
        wlr_cursor_absolute_to_layout_coords(cursor, &e->touch->base, e->x, e->y, &lx, &ly);
        if (only)
            wlr_cursor_warp_absolute(cursor, &e->touch->base, e->x, e->y);
        wlr_seat_touch_notify_motion(wlr, e->time_msec, e->touch_id, lx - it->ox, ly - it->oy);
    } else if (only) {
        emulate_absolute(&e->touch->base, e->x, e->y, e->time_msec);
    }
}

void Seat::touch_up(wlr_touch_up_event* e) {
    server.note_activity();
    auto it = std::ranges::find(touch_points_, e->touch_id, &TouchPoint::id);
    if (it == touch_points_.end())
        return;
    if (it->surface)
        wlr_seat_touch_notify_up(wlr, e->time_msec, e->touch_id);
    else
        emulate_button(BTN_LEFT, false, e->time_msec);
    touch_points_.erase(it);
}

// --- tablet tools -------------------------------------------------------------

TabletTool* Seat::tool_for(wlr_tablet_tool* tool) {
    if (tool->data)
        return static_cast<TabletTool*>(tool->data);
    auto t = std::make_unique<TabletTool>();
    t->tool = tool;
    t->v2 = wlr_tablet_tool_create(server.tablet_manager, wlr, tool);
    // Mice and lenses on a tablet move like mice; apps rarely handle them as
    // tablet tools, and they have nothing a pen has (labwc).
    t->relative = tool->type == WLR_TABLET_TOOL_TYPE_MOUSE || tool->type == WLR_TABLET_TOOL_TYPE_LENS;
    t->emulate = tool->type == WLR_TABLET_TOOL_TYPE_MOUSE;
    tool->data = t.get();
    TabletTool* rt = t.get();
    // The app under the pen sets its cursor.
    t->set_cursor.connect(&t->v2->events.set_cursor, [this, rt](wlr_tablet_v2_event_cursor* e) {
        wlr_surface* on = rt->v2->focused_surface;
        if (on && e->seat_client && e->seat_client->client == wl_resource_get_client(on->resource))
            wlr_cursor_set_surface(cursor, e->surface, e->hotspot_x, e->hotspot_y);
    });
    t->destroy.connect(&tool->events.destroy, [this, rt](void*) {
        rt->tool->data = nullptr;
        std::erase_if(tools_, [rt](const auto& x) { return x.get() == rt; });
    });
    tools_.push_back(std::move(t));
    return rt;
}

void Seat::tool_proximity(wlr_tablet_tool_proximity_event* e) {
    server.note_activity();
    auto* tablet = static_cast<TabletDevice*>(e->tablet->data);
    TabletTool* tool = tool_for(e->tool);
    typing_hidden_ = false;
    tool->x = e->x;
    tool->y = e->y;
    if (!tablet || !tablet->v2 || tool->emulate || tool->relative)
        return;
    double lx, ly, ox, oy;
    wlr_cursor_absolute_to_layout_coords(cursor, &e->tablet->base, e->x, e->y, &lx, &ly);
    wlr_surface* surface = touch_target(lx, ly, ox, oy, true);
    if (e->state == WLR_TABLET_TOOL_PROXIMITY_OUT) {
        wlr_tablet_v2_tablet_tool_notify_proximity_out(tool->v2);
        return;
    }
    if (!surface)
        return;
    wlr_cursor_warp_absolute(cursor, &e->tablet->base, e->x, e->y);
    // The pointer leaves while the pen is there (as GNOME does), so the app
    // isn't torn between the two.
    wlr_seat_pointer_notify_clear_focus(wlr);
    wlr_tablet_v2_tablet_tool_notify_proximity_in(tool->v2, tablet->v2, surface);
    wlr_tablet_v2_tablet_tool_notify_motion(tool->v2, lx - ox, ly - oy);
}

void Seat::tool_axis(wlr_tablet_tool_axis_event* e) {
    server.note_activity();
    auto* tablet = static_cast<TabletDevice*>(e->tablet->data);
    TabletTool* tool = tool_for(e->tool);
    typing_hidden_ = false;
    double dx = 0, dy = 0;
    if (e->updated_axes & WLR_TABLET_TOOL_AXIS_X) {
        tool->x = e->x;
        dx = e->dx;
    }
    if (e->updated_axes & WLR_TABLET_TOOL_AXIS_Y) {
        tool->y = e->y;
        dy = e->dy;
    }

    double lx, ly;
    if (tool->relative) {
        lx = cursor->x + dx;
        ly = cursor->y + dy;
    } else {
        wlr_cursor_absolute_to_layout_coords(cursor, &e->tablet->base, tool->x, tool->y, &lx, &ly);
    }
    double ox = 0, oy = 0;
    wlr_surface* surface = (tablet && tablet->v2 && !tool->emulate) ? touch_target(lx, ly, ox, oy, true) : nullptr;
    const bool grabbed = wlr_tablet_tool_v2_has_implicit_grab(tool->v2);
    if (grabbed) {
        // The tip is down: everything stays with where it went down.
        surface = tool->v2->focused_surface;
        ox = tool->grab_ox;
        oy = tool->grab_oy;
    }

    if (!pen_emulating_ && surface) {
        if (surface != tool->v2->focused_surface && !tool->v2->is_down) {
            wlr_seat_pointer_notify_clear_focus(wlr);
            wlr_tablet_v2_tablet_tool_notify_proximity_in(tool->v2, tablet->v2, surface);
        }
        if (tool->relative)
            wlr_cursor_move(cursor, &e->tablet->base, dx, dy);
        else
            wlr_cursor_warp_absolute(cursor, &e->tablet->base, tool->x, tool->y);
        wlr_tablet_v2_tablet_tool_notify_motion(tool->v2, lx - ox, ly - oy);
        if (e->tool->pressure && (e->updated_axes & WLR_TABLET_TOOL_AXIS_PRESSURE))
            wlr_tablet_v2_tablet_tool_notify_pressure(tool->v2, e->pressure);
        if (e->tool->distance && (e->updated_axes & WLR_TABLET_TOOL_AXIS_DISTANCE))
            wlr_tablet_v2_tablet_tool_notify_distance(tool->v2, e->distance);
        if (e->tool->tilt && (e->updated_axes & (WLR_TABLET_TOOL_AXIS_TILT_X | WLR_TABLET_TOOL_AXIS_TILT_Y)))
            wlr_tablet_v2_tablet_tool_notify_tilt(tool->v2, e->tilt_x, e->tilt_y);
        if (e->tool->rotation && (e->updated_axes & WLR_TABLET_TOOL_AXIS_ROTATION))
            wlr_tablet_v2_tablet_tool_notify_rotation(tool->v2, e->rotation);
        if (e->tool->slider && (e->updated_axes & WLR_TABLET_TOOL_AXIS_SLIDER))
            wlr_tablet_v2_tablet_tool_notify_slider(tool->v2, e->slider);
        if (e->tool->wheel && (e->updated_axes & WLR_TABLET_TOOL_AXIS_WHEEL))
            wlr_tablet_v2_tablet_tool_notify_wheel(tool->v2, e->wheel_delta, 0);
        return;
    }

    // Working the pointer.
    if (e->updated_axes & (WLR_TABLET_TOOL_AXIS_X | WLR_TABLET_TOOL_AXIS_Y)) {
        if (tool->v2->focused_surface)
            wlr_tablet_v2_tablet_tool_notify_proximity_out(tool->v2);
        if (tool->relative)
            motion(e->time_msec, &e->tablet->base, dx, dy, dx, dy);
        else
            emulate_absolute(&e->tablet->base, tool->x, tool->y, e->time_msec);
    }
    if (e->updated_axes & WLR_TABLET_TOOL_AXIS_WHEEL) {
        wlr_pointer_axis_event a{};
        a.time_msec = e->time_msec;
        a.source = WL_POINTER_AXIS_SOURCE_WHEEL;
        a.orientation = WL_POINTER_AXIS_VERTICAL_SCROLL;
        a.delta = e->wheel_delta;
        a.delta_discrete = (e->wheel_delta >= 0 ? 1 : -1) * WLR_POINTER_AXIS_DISCRETE_STEP;
        axis(&a);
        wlr_seat_pointer_notify_frame(wlr);
    }
}

void Seat::tool_tip(wlr_tablet_tool_tip_event* e) {
    server.note_activity();
    auto* tablet = static_cast<TabletDevice*>(e->tablet->data);
    TabletTool* tool = tool_for(e->tool);
    const bool down = e->state == WLR_TABLET_TOOL_TIP_DOWN;
    double lx = cursor->x, ly = cursor->y, ox = 0, oy = 0;
    wlr_surface* surface = (tablet && tablet->v2 && !tool->emulate) ? touch_target(lx, ly, ox, oy, true) : nullptr;

    if (!pen_emulating_ && (surface || wlr_tablet_tool_v2_has_implicit_grab(tool->v2))) {
        if (down) {
            focus_for_touch(server.hit_test(lx, ly));
            tool->grab_ox = ox;
            tool->grab_oy = oy;
            wlr_tablet_v2_tablet_tool_notify_down(tool->v2);
            wlr_tablet_tool_v2_start_implicit_grab(tool->v2);
        } else {
            wlr_tablet_v2_tablet_tool_notify_up(tool->v2);
            // Lifted off somewhere that doesn't take a pen.
            if (!surface)
                wlr_tablet_v2_tablet_tool_notify_proximity_out(tool->v2);
        }
        return;
    }
    pen_emulating_ = down;
    emulate_button(BTN_LEFT, down, e->time_msec);
}

void Seat::tool_button(wlr_tablet_tool_button_event* e) {
    server.note_activity();
    auto* tablet = static_cast<TabletDevice*>(e->tablet->data);
    TabletTool* tool = tool_for(e->tool);
    const bool pressed = e->state == WLR_BUTTON_PRESSED;
    double ox, oy;
    wlr_surface* surface =
        (tablet && tablet->v2 && !tool->emulate) ? touch_target(cursor->x, cursor->y, ox, oy, true) : nullptr;
    if (!pen_emulating_ && surface) {
        wlr_tablet_v2_tablet_tool_notify_button(tool->v2, e->button,
            pressed ? ZWP_TABLET_PAD_V2_BUTTON_STATE_PRESSED : ZWP_TABLET_PAD_V2_BUTTON_STATE_RELEASED);
        return;
    }
    if (uint32_t b = tool->emulate ? e->button : mouse_button_for(e->button)) {
        pen_emulating_ = pressed;
        emulate_button(b, pressed, e->time_msec);
    }
}

} // namespace atrium
