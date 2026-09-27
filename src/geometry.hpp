#pragma once
// Window geometry rules, free of any compositor state so they can be tested
// on their own.

extern "C" {
#include <wlr/util/box.h>
#include <wlr/util/edges.h>
}

#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace atrium::geometry {

// A miniature's piece (a buffer at `box` in the window's frame, showing
// `src` of its buffer, which is `buffer_w` x `buffer_h`) clipped to the frame
// (0, 0, frame_w, frame_h): what an app draws past its frame (its shadow)
// is cut off, and the source crop follows. Nothing when it's all outside.
struct ClippedPiece {
    wlr_box box;
    wlr_fbox src;
};
std::optional<ClippedPiece> clip_to_frame(const wlr_box& box, const wlr_fbox& src, int buffer_w, int buffer_h,
                                          int frame_w, int frame_h);

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

// Window commands (the palette's, after Rectangle): where `name` puts a window
// now at `current` inside `area`, with `gap` around and between. Halves,
// quarters, thirds, fourths, sixths, centered sizes, edges ("move-left" keeps
// the size), "size:0.6x0.5" (a share of the area, centered). Pressing a half
// again steps it to two thirds, then one third. Nothing for an unknown name,
// or one the window manager answers itself (maximize, restore, displays).
std::optional<wlr_box> named_place(std::string_view name, const wlr_box& area, const wlr_box& current, int gap,
                                   bool again = true);
// Every name named_place() knows, in the palette's order.
std::span<const std::string_view> place_names();

// Where a window in a secret space (or a tiled one) goes: `area` (what a
// maximized window fills) less a margin of `percent` of its shorter side,
// the same on every side.
wlr_box secret_frame(const wlr_box& area, int percent);

// Bring a remembered window box back onto `area`: shrink it to fit, then
// slide it in until it is fully inside.
wlr_box fit_into(wlr_box box, const wlr_box& area);

// Tiling, dwindle-style: the first window takes the whole `area`; each
// next one halves the space the one before it had, along its longer side
// (the newer window right or below). `gap` goes between windows.
std::vector<wlr_box> dwindle(size_t count, const wlr_box& area, int gap);

// The window next to `from` in a direction (WLR_EDGE_LEFT/RIGHT/TOP/BOTTOM):
// of the ones whose center lies that way, the nearest, with sideways
// distance counting double so straight ahead wins. -1 when there is none.
int neighbor(const wlr_box& from, std::span<const wlr_box> others, uint32_t direction);

// Overview: every window scaled into `area` as rows of equal height, keeping
// their rough arrangement (top rows stay on top, left stays left) and never
// enlarged. `gap` separates windows and rows; `label` is extra room kept
// under each row for titles. Returns one box per window, in input order.
std::vector<wlr_box> overview_layout(std::span<const wlr_box> windows, const wlr_box& area,
                                     int gap, int label);

} // namespace atrium::geometry
