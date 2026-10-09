#pragma once
// The devices behind touch_tablet.cpp, for Seat to hold.
#include "listener.hpp"
#include "wlr.hpp"

#include <string>

namespace atrium {

// A touchscreen or tablet, kept to map it to its screen and to forget it.
struct InputDevice {
    wlr_input_device* device;
    std::string output;  // the screen it belongs to, if it says
    Listener<> destroy;
};

struct TabletDevice {
    wlr_tablet* tablet;
    wlr_tablet_v2_tablet* v2;
    Listener<> destroy;
};

struct TabletPad {
    wlr_tablet_pad* pad;
    wlr_tablet_v2_tablet_pad* v2;
    TabletDevice* tablet = nullptr;  // the tablet it is part of (same libinput group)
    wlr_surface* surface = nullptr;  // where its buttons go
    Listener<wlr_tablet_pad_button_event> button;
    Listener<wlr_tablet_pad_ring_event> ring;
    Listener<wlr_tablet_pad_strip_event> strip;
    Listener<> surface_destroy, destroy;
};

struct TabletTool {
    wlr_tablet_tool* tool;
    wlr_tablet_v2_tablet_tool* v2;
    bool relative = false;  // a tablet mouse or lens moves the pointer as a mouse
    bool emulate = false;   // a tablet mouse always works the pointer
    double x = 0, y = 0;    // 0..1 across the tablet
    double grab_ox = 0, grab_oy = 0;  // the surface's offset while the tip is down
    Listener<wlr_tablet_v2_event_cursor> set_cursor;
    Listener<> destroy;
};

} // namespace atrium
