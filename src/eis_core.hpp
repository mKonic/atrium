#pragma once
// Input capture's pointer barriers (the InputCapture portal, KWin's
// eisinputcapturemanager.cpp), without wlroots: which barriers an app may set
// on the screens' outer edges, and when the pointer pushes through one.

#include <cstdint>
#include <optional>
#include <vector>

namespace atrium::eis {

struct Zone {
    int x, y, width, height;  // a screen, in layout coordinates
};

// A line from (x1, y1) to (x2, y2), as the portal passes it.
struct Barrier {
    uint32_t id = 0;
    int x1 = 0, y1 = 0, x2 = 0, y2 = 0;
};

// A barrier must run along part of a screen's edge that no other screen
// touches (pushing through it leaves the desktop), and be straight.
bool valid(const Barrier& b, const std::vector<Zone>& zones);

// The barrier the pointer at (x, y) pushes through by moving (dx, dy), if
// any: it lies along the barrier and its move would cross the line.
std::optional<uint32_t> crossed(const std::vector<Barrier>& barriers, double x, double y, double dx, double dy);

// The xdg-desktop-portal keyboard, pointer and touchscreen bits.
enum Devices : uint32_t { Keyboard = 1, Pointer = 2, Touchscreen = 4 };

} // namespace atrium::eis
