#pragma once
// Warping a window: a function from a point of its frame (u, v in 0..1) to
// where it lands, drawn as a grid of triangles. Free of compositor state so
// it can be tested on its own.

#include <functional>
#include <utility>
#include <vector>

namespace atrium::warp {

// Where frame point (u, v) lands, in layout pixels.
using Fn = std::function<std::pair<double, double>(double u, double v)>;

struct Vertex {
    float x, y;  // where it lands (layout pixels)
    float u, v;  // which point of the piece it is (0..1 across the piece)
};

// Triangles (6 per cell, `cells` x `cells`) for a piece covering frame
// fractions [u0, u1] x [v0, v1].
std::vector<Vertex> mesh(const Fn& fn, double u0, double v0, double u1, double v1, int cells);

// macOS's genie: the frame (x, y, width, height) pours into a Dock icon
// `icon` wide centred at (to_x, to_y) as t goes 0 to 1. The first half
// stretches its bottom down into the icon, narrowing it into a funnel; the
// second half slides the rest in.
std::pair<double, double> genie(double x, double y, double width, double height, double to_x, double to_y,
                                double icon, double t, double u, double v);

} // namespace atrium::warp
