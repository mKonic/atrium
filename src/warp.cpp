#include "warp.hpp"

#include <algorithm>
#include <cmath>

namespace atrium::warp {

std::vector<Vertex> mesh(const Fn& fn, double u0, double v0, double u1, double v1, int cells) {
    cells = std::max(1, cells);
    std::vector<Vertex> grid;
    grid.reserve(size_t(cells + 1) * size_t(cells + 1));
    for (int j = 0; j <= cells; ++j)
        for (int i = 0; i <= cells; ++i) {
            const double s = double(i) / cells, t = double(j) / cells;
            const auto [x, y] = fn(u0 + (u1 - u0) * s, v0 + (v1 - v0) * t);
            grid.push_back({float(x), float(y), float(s), float(t)});
        }
    std::vector<Vertex> out;
    out.reserve(size_t(cells) * size_t(cells) * 6);
    const auto at = [&](int i, int j) { return grid[size_t(j) * size_t(cells + 1) + size_t(i)]; };
    for (int j = 0; j < cells; ++j)
        for (int i = 0; i < cells; ++i) {
            out.push_back(at(i, j));
            out.push_back(at(i, j + 1));
            out.push_back(at(i + 1, j));
            out.push_back(at(i + 1, j));
            out.push_back(at(i, j + 1));
            out.push_back(at(i + 1, j + 1));
        }
    return out;
}

std::pair<double, double> genie(double x, double y, double width, double height, double to_x, double to_y,
                                double icon, double t, double u, double v) {
    t = std::clamp(t, 0.0, 1.0);
    const double squeeze = std::min(1.0, t * 2);        // the funnel forms
    const double slide = std::max(0.0, t * 2 - 1);      // and everything pours in
    const double span = std::max(1.0, to_y - y);        // top of the frame to the icon
    // The lower part stretches down to the icon first (v² keeps the top
    // steady), then everything slides into it.
    double py = y + v * height + squeeze * v * v * (to_y - (y + height));
    py += slide * (to_y - py);
    // How far up the funnel: 1 at the frame's top, 0 at the icon; the
    // narrowing eases in (smoothstep), so the top keeps its width longest.
    const double k = std::clamp((to_y - py) / span, 0.0, 1.0);
    const double keep = 1 - squeeze * (1 - k * k * (3 - 2 * k));
    const double w = icon + (width - icon) * keep;
    const double left = (to_x - icon / 2) + (x - (to_x - icon / 2)) * keep;
    return {left + u * w, py};
}

} // namespace atrium::warp
