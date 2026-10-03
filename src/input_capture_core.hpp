#pragma once
// Input capture's pointer barriers (the InputCapture portal), apart from
// the compositor: lines on the outside edge of the desktop the pointer
// "leaves" through to another computer (Deskflow, Input Leap). A barrier
// lies on the top (horizontal) or left (vertical) edge of its pixels, both
// ends included: on a 1920-wide screen at 0,0 the right edge is x=1920.

#include "util/box.hpp"

#include <cstdint>
#include <optional>
#include <span>

namespace atrium::input_capture {

struct Barrier {
    uint32_t id = 0;
    int x1 = 0, y1 = 0, x2 = 0, y2 = 0;
};

// Whether `b` is a barrier the portal allows on these screens: horizontal
// or vertical, on one screen's edge, with no screen beyond it.
bool valid(const Barrier& b, std::span<const Box> screens);

// The barrier the pointer goes through moving from (x0, y0) by (dx, dy),
// and where it would be (past the edge, as the portal reports it).
struct Crossing {
    uint32_t id;
    double x, y;
};
std::optional<Crossing> crossing(std::span<const Barrier> barriers, double x0, double y0, double dx, double dy);

} // namespace atrium::input_capture
