#pragma once
// Window geometry rules, free of any compositor state so they can be tested
// on their own.

extern "C" {
#include <wlr/util/box.h>
#include <wlr/util/edges.h>
}

#include <span>

namespace atrium::geometry {

// Where a new window of size (w, h) opens inside `area`.
//  - Over its parent, centered, when it has one.
//  - Otherwise centered, stepped down-right by `step` while it would land
//    within `step` of an existing window's top-left, wrapping to the area's
//    top-left when a step would push it off the bottom or right.
// The result always fits in `area` (size clamped, position clamped).
wlr_box place(int w, int h, const wlr_box& area, const wlr_box* parent,
              std::span<const wlr_box> others, int step);

// Stick a dragged window's top-left to the edges of `area` when within
// `distance` of one. Left/top win over right/bottom.
void snap(int& x, int& y, int w, int h, const wlr_box& area, int distance);

// The box an edge/corner resize produces from the box at grab start and the
// cursor's travel since. Never smaller than 1x1; the moving edge is the one
// that gives way.
wlr_box resize(const wlr_box& start, uint32_t edges, int dx, int dy);

// Clamp a requested size to a client's min/max hints. A zero or negative
// hint means "no limit".
wlr_box clamp_to_hints(wlr_box box, const wlr_box& min, const wlr_box& max);

// Which corner Mod + right-drag resizes from: the quarter of the window the
// cursor is in.
uint32_t nearest_corner(const wlr_box& window, double cx, double cy);

} // namespace atrium::geometry
