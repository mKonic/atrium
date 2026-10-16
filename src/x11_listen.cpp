// KWin's "Legacy X11 App Support": keys (and extra mouse buttons) pressed
// in other apps' windows also go to Xwayland, so X11 apps' global shortcuts
// (push-to-talk in an X11 Discord, OBS, TeamSpeak) work in the background.
// Specced from KWin's XwaylandInputFilter (src/xwayland/xwayland.cpp): after
// the desktop's own shortcuts, before the input method; a key whose press
// went also sends its release; when Xwayland gets focus, what it heard is let
// go first. Off by default (keyboard.x11_listen).
#include "seat.hpp"
#include "server.hpp"
#include "x11_listen_core.hpp"

#include <linux/input-event-codes.h>

namespace atrium {

namespace {

wlr_seat_client* xwayland_client(Server& server, wlr_seat* seat) {
    if (!server.xwayland || !server.xwayland->server || !server.xwayland->server->client)
        return nullptr;
    return wlr_seat_client_for_wl_client(seat, server.xwayland->server->client);
}

} // namespace

void Seat::x11_listen_key(wlr_keyboard* kb, const wlr_keyboard_key_event* e, uint32_t keysym) {
    wlr_seat_client* xw = xwayland_client(server, wlr);
    if (!xw)
        return;
    // Xwayland has the keyboard: it hears everything anyway.
    if (wlr_surface* focus = wlr->keyboard_state.focused_surface;
        focus && wl_resource_get_client(focus->resource) == xw->client)
        return;
    const bool pressed = e->state == WL_KEYBOARD_KEY_STATE_PRESSED;
    const uint32_t mods = wlr_keyboard_get_modifiers(kb);
    const bool combo = mods & (WLR_MODIFIER_CTRL | WLR_MODIFIER_ALT | WLR_MODIFIER_LOGO);
    // A key it heard pressed, it hears let go, whatever the rule says now.
    if (pressed) {
        if (x11_pressed_.contains(e->keycode) || !x11_listen::hears(server.config.x11_listen_keys, keysym, combo))
            return;
        x11_pressed_.insert(e->keycode);
    } else if (!x11_pressed_.erase(e->keycode)) {
        return;
    }
    const uint32_t serial = wl_display_next_serial(server.display);
    wl_resource* r;
    wl_resource_for_each(r, &xw->keyboards) {
        wl_keyboard_send_key(r, serial, e->time_msec, e->keycode, e->state);
    }
    const wlr_keyboard_modifiers& m = kb->modifiers;
    if (m.depressed != x11_mods_.depressed || m.latched != x11_mods_.latched || m.locked != x11_mods_.locked ||
        m.group != x11_mods_.group) {
        x11_mods_ = m;
        wl_resource_for_each(r, &xw->keyboards) {
            wl_keyboard_send_modifiers(r, serial, m.depressed, m.latched, m.locked, m.group);
        }
    }
}

void Seat::x11_listen_button(uint32_t time, uint32_t button, wl_pointer_button_state state) {
    if (!server.config.x11_listen_buttons || button == BTN_LEFT || button == BTN_RIGHT || button == BTN_MIDDLE)
        return;
    wlr_seat_client* xw = xwayland_client(server, wlr);
    if (!xw)
        return;
    if (wlr_surface* focus = wlr->pointer_state.focused_surface;
        focus && wl_resource_get_client(focus->resource) == xw->client)
        return;
    const uint32_t serial = wl_display_next_serial(server.display);
    wl_resource* r;
    wl_resource_for_each(r, &xw->pointers) {
        wl_pointer_send_button(r, serial, time, button, state);
        if (wl_resource_get_version(r) >= WL_POINTER_FRAME_SINCE_VERSION)
            wl_pointer_send_frame(r);
    }
}

void Seat::x11_listen_focus(wlr_surface* surface) {
    // Xwayland gets the keyboard back: the keys it heard pressed are let go
    // first, so none sticks (KWin does the same).
    wlr_seat_client* xw = xwayland_client(server, wlr);
    if (!xw || !surface || wl_resource_get_client(surface->resource) != xw->client || x11_pressed_.empty())
        return;
    const uint32_t serial = wl_display_next_serial(server.display);
    wl_resource* r;
    for (uint32_t key : x11_pressed_)
        wl_resource_for_each(r, &xw->keyboards) {
            wl_keyboard_send_key(r, serial, 0, key, WL_KEYBOARD_KEY_STATE_RELEASED);
        }
    x11_pressed_.clear();
    x11_mods_ = {};
}

} // namespace atrium
