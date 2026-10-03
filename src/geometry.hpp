#pragma once
// Window geometry rules, free of any compositor state so they can be tested
// on their own.

#include "util/box.hpp"

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
    Box box;
    FBox src;
};
std::optional<ClippedPiece> clip_to_frame(const Box& box, const FBox& src, int buffer_w, int buffer_h,
                                          int frame_w, int frame_h);

// Where a new window of size (w, h) opens inside `area`.
//  - Over its parent, centered, when it has one.
//  - Otherwise centered, stepped down-right by `step` while it would land
//    within `step` of an existing window's top-left, wrapping to the area's
//    top-left when a step would push it off the bottom or right.
// The result always fits in `area` (size clamped, position clamped).
Box place(int w, int h, const Box& area, const Box* parent,
              std::span<const Box> others, int step);

// Stick a dragged window's top-left to the edges of `area` when within
// `distance` of one. Left/top win over right/bottom.
void snap(int& x, int& y, int w, int h, const Box& area, int distance);

// The box an edge/corner resize produces from the box at grab start and the
// cursor's travel since. Never smaller than 1x1; the moving edge is the one
// that gives way.
Box resize(const Box& start, uint32_t edges, int dx, int dy);

// Clamp a requested size to a client's min/max hints. A zero or negative
// hint means "no limit".
Box clamp_to_hints(Box box, const Box& min, const Box& max);

// Which corner Mod + right-drag resizes from: the quarter of the window the
// cursor is in.
uint32_t nearest_corner(const Box& window, double cx, double cy);

// Snapping: which part of `area` a window dragged with the cursor at (cx, cy)
// wants. Returns EDGE_* bits: LEFT or RIGHT alone for a half, a LEFT/RIGHT
// + TOP/BOTTOM pair for a quarter, TOP alone for the whole area, 0 for none.
// `edge` is how close to the screen edge the cursor must be; `corner` is how
// far along an edge from a corner still counts as that corner.
uint32_t snap_zone(const Box& area, double cx, double cy, int edge, int corner);

// The box a snap zone stands for, with `gap` between snapped windows and
// around them. TOP alone (maximize) fills the area without a gap.
Box snap_box(const Box& area, uint32_t zone, int gap);

// Window commands (the palette's, after Rectangle): where `name` puts a window
// now at `current` inside `area`, with `gap` around and between. Halves,
// quarters, thirds, fourths, sixths, centered sizes, edges ("move-left" keeps
// the size), "size:0.6x0.5" (a share of the area, centered). Pressing a half
// again steps it to two thirds, then one third. Nothing for an unknown name,
// or one the window manager answers itself (maximize, restore, displays).
std::optional<Box> named_place(std::string_view name, const Box& area, const Box& current, int gap,
                                   bool again = true);
// Every name named_place() knows, in the palette's order.
std::span<const std::string_view> place_names();

// Where a window in a secret space (or a tiled one) goes: `area` (what a
// maximized window fills) less a margin of `percent` of its shorter side,
// the same on every side.
Box secret_frame(const Box& area, int percent);

// Bring a remembered window box back onto `area`: shrink it to fit, then
// slide it in until it is fully inside.
Box fit_into(Box box, const Box& area);

// Tiling, dwindle-style: the first window takes the whole `area`; each
// next one halves the space the one before it had, along its longer side
// (the newer window right or below). `gap` goes between windows.
std::vector<Box> dwindle(size_t count, const Box& area, int gap);

// The window next to `from` in a direction (EDGE_LEFT/RIGHT/TOP/BOTTOM):
// of the ones whose center lies that way, the nearest, with sideways
// distance counting double so straight ahead wins. -1 when there is none.
int neighbor(const Box& from, std::span<const Box> others, uint32_t direction);

// Overview: every window scaled into `area` as rows of equal height, keeping
// their rough arrangement (top rows stay on top, left stays left) and never
// enlarged. `gap` separates windows and rows; `label` is extra room kept
// under each row for titles. Returns one box per window, in input order.
std::vector<Box> overview_layout(std::span<const Box> windows, const Box& area,
                                     int gap, int label);

// A laptop's own panel, by its connector's name (eDP-1, LVDS-1, DSI-1).
bool internal_panel(std::string_view connector);

// The scale a screen starts at when nothing was set for it: the quarter
// step that brings it nearest a comfortable density (a laptop's panel is
// seen closer, so denser: 135 dpi; a monitor 110), from 1 to 3. 1 when its
// size is unknown or not believable (a projector, an EDID with only its
// aspect ratio).
double default_scale(int width_px, int height_px, int width_mm, int height_mm, bool built_in);

} // namespace atrium::geometry
