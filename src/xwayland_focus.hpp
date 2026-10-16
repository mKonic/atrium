#pragma once
#ifdef ATRIUM_XWAYLAND
// Keyboard focus crossing between X11 and native Wayland windows.
//
// Leaving X11: wlroots hands X focus to PointerRoot, which still lets an X11
// launcher (Steam) believe it is the active window and keep a native game's
// controller input. X focus goes to a window of the XWM's own instead
// (wlr_xwayland_focus_nothing, KWin's Workspace::focusToNull): passive grabs
// on the root (push-to-talk in an X11 Discord, keyboard.x11_listen) still
// fire, keys reach no app, and _NET_ACTIVE_WINDOW says None. Done on the next loop turn,
// after the native client's keyboard enter has gone out (Umbriel's
// xwayland-input-stability.md: clearing first can leave the game without
// its controller).
//
// Entering X11: wlroots refuses X11 selection reads while a native window is
// focused, and Wine reads the clipboard's targets eagerly when the owner
// changes, so its cache stays empty after a copy made elsewhere. Rebinding
// the seat announces the native selections again once X11 has focus
// (Umbriel's Xwayland::onSelectionRefreshTimer).
#include "listener.hpp"
#include "wlr.hpp"

namespace atrium {

class Server;

class XwaylandFocus {
public:
    explicit XwaylandFocus(Server& server);
    ~XwaylandFocus();

private:
    void focus_changed(wlr_surface* old_surface, wlr_surface* new_surface);
    void leave_x11();
    void refresh_selection();

    Server& server_;
    Listener<wlr_seat_keyboard_focus_change_event> focus_change_;
    wl_event_source* leave_timer_ = nullptr;
    wl_event_source* selection_timer_ = nullptr;
};

} // namespace atrium
#endif
