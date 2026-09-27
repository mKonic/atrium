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

Wobble::Wobble(int points) : n_(std::max(2, points)), points_(size_t(n_) * size_t(n_)) {}

void Wobble::moved(double dx, double dy, double gu, double gv) {
    gu = std::clamp(gu, 0.0, 1.0);
    gv = std::clamp(gv, 0.0, 1.0);
    const double reach = std::max(std::hypot(std::max(gu, 1 - gu), std::max(gv, 1 - gv)), 1e-3);
    for (int j = 0; j < n_; ++j)
        for (int i = 0; i < n_; ++i) {
            const double u = double(i) / (n_ - 1), v = double(j) / (n_ - 1);
            const double far = std::clamp(std::hypot(u - gu, v - gv) / reach, 0.0, 1.0);
            Point& p = points_[size_t(j) * size_t(n_) + size_t(i)];
            p.x -= dx * far;
            p.y -= dy * far;
            const double len = std::hypot(p.x, p.y);
            if (len > kMaxLag) {
                p.x *= kMaxLag / len;
                p.y *= kMaxLag / len;
            }
        }
}

void Wobble::advance(double seconds) {
    // Small fixed steps: stable at any frame rate.
    constexpr double kStep = 1.0 / 240;
    for (double left = std::clamp(seconds, 0.0, 0.1); left > 0; left -= kStep) {
        const double dt = std::min(kStep, left);
        for (Point& p : points_) {
            p.vx += (-kStiffness * p.x - kDamping * p.vx) * dt;
            p.vy += (-kStiffness * p.y - kDamping * p.vy) * dt;
            p.x += p.vx * dt;
            p.y += p.vy * dt;
        }
    }
}

bool Wobble::stable() const {
    return std::ranges::all_of(points_, [](const Point& p) {
        return std::abs(p.x) < 0.3 && std::abs(p.y) < 0.3 && std::abs(p.vx) < 3 && std::abs(p.vy) < 3;
    });
}

std::pair<double, double> Wobble::offset(double u, double v) const {
    const double fx = std::clamp(u, 0.0, 1.0) * (n_ - 1), fy = std::clamp(v, 0.0, 1.0) * (n_ - 1);
    const int i = std::min(int(fx), n_ - 2), j = std::min(int(fy), n_ - 2);
    const double a = fx - i, b = fy - j;
    const auto at = [&](int x, int y) -> const Point& { return points_[size_t(y) * size_t(n_) + size_t(x)]; };
    const Point &p00 = at(i, j), &p10 = at(i + 1, j), &p01 = at(i, j + 1), &p11 = at(i + 1, j + 1);
    const double x = (p00.x * (1 - a) + p10.x * a) * (1 - b) + (p01.x * (1 - a) + p11.x * a) * b;
    const double y = (p00.y * (1 - a) + p10.y * a) * (1 - b) + (p01.y * (1 - a) + p11.y * a) * b;
    return {x, y};
}

} // namespace atrium::warp
