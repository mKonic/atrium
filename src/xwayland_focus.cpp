#ifdef ATRIUM_XWAYLAND
#include "xwayland_focus.hpp"
#include "seat.hpp"
#include "server.hpp"

namespace atrium {

namespace {

bool from_xwayland(Server& server, wlr_surface* surface) {
    return surface && server.xwayland && server.xwayland->server && server.xwayland->server->client &&
           wl_resource_get_client(surface->resource) == server.xwayland->server->client;
}

// One millisecond: a zero timer is disarmed, and this puts the work on the
// next loop turn, after the current dispatch has flushed.
void next_turn(wl_event_source* timer) {
    if (timer)
        wl_event_source_timer_update(timer, 1);
}

} // namespace

XwaylandFocus::XwaylandFocus(Server& server) : server_(server) {
    wl_event_loop* loop = wl_display_get_event_loop(server.display);
    leave_timer_ = wl_event_loop_add_timer(loop, [](void* data) {
        static_cast<XwaylandFocus*>(data)->leave_x11();
        return 0;
    }, this);
    selection_timer_ = wl_event_loop_add_timer(loop, [](void* data) {
        static_cast<XwaylandFocus*>(data)->refresh_selection();
        return 0;
    }, this);
    focus_change_.connect(&server.seat->wlr->keyboard_state.events.focus_change,
        [this](wlr_seat_keyboard_focus_change_event* e) { focus_changed(e->old_surface, e->new_surface); });
}

XwaylandFocus::~XwaylandFocus() {
    if (leave_timer_)
        wl_event_source_remove(leave_timer_);
    if (selection_timer_)
        wl_event_source_remove(selection_timer_);
}

void XwaylandFocus::focus_changed(wlr_surface* old_surface, wlr_surface* new_surface) {
    const bool was_x11 = from_xwayland(server_, old_surface);
    const bool is_x11 = from_xwayland(server_, new_surface);
    if (was_x11 && new_surface && !is_x11)
        next_turn(leave_timer_);
    else if (is_x11 && !was_x11)
        next_turn(selection_timer_);
}

void XwaylandFocus::leave_x11() {
    // Focus may have come back to X11 meanwhile: then wlroots' focus stands.
    wlr_surface* focused = server_.seat->wlr->keyboard_state.focused_surface;
    if (focused && !from_xwayland(server_, focused))
        wlr_xwayland_focus_nothing(server_.xwayland);
}

void XwaylandFocus::refresh_selection() {
    if (!from_xwayland(server_, server_.seat->wlr->keyboard_state.focused_surface) ||
        !wlr_xwayland_get_xwm_connection(server_.xwayland))
        return;
    // The same seat again: wlroots re-announces the native clipboard and
    // primary selection, and leaves X11-owned ones as they are.
    wlr_xwayland_set_seat(server_.xwayland, server_.seat->wlr);
}

} // namespace atrium
#endif
