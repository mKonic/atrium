#pragma once
// Window geometry rules, free of any compositor state so they can be tested
// on their own.

extern "C" {
#include <wlr/util/box.h>
#include <wlr/util/edges.h>
}

#include <span>
#include <vector>

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

// Snapping: which part of `area` a window dragged with the cursor at (cx, cy)
// wants. Returns WLR_EDGE_* bits: LEFT or RIGHT alone for a half, a LEFT/RIGHT
// + TOP/BOTTOM pair for a quarter, TOP alone for the whole area, 0 for none.
// `edge` is how close to the screen edge the cursor must be; `corner` is how
// far along an edge from a corner still counts as that corner.
uint32_t snap_zone(const wlr_box& area, double cx, double cy, int edge, int corner);

// The box a snap zone stands for, with `gap` between snapped windows and
// around them. TOP alone (maximize) fills the area without a gap.
wlr_box snap_box(const wlr_box& area, uint32_t zone, int gap);

// Where a window in a secret space goes: the screen less `percent` of it
// on every side, so the blurred desktop shows around the window.
wlr_box secret_frame(const wlr_box& output, int percent);

// Bring a remembered window box back onto `area`: shrink it to fit, then
// slide it in until it is fully inside.
wlr_box fit_into(wlr_box box, const wlr_box& area);

// Overview: every window scaled into `area` as rows of equal height, keeping
// their rough arrangement (top rows stay on top, left stays left) and never
// enlarged. `gap` separates windows and rows; `label` is extra room kept
// under each row for titles. Returns one box per window, in input order.
std::vector<wlr_box> overview_layout(std::span<const wlr_box> windows, const wlr_box& area,
                                     int gap, int label);

} // namespace atrium::geometry
