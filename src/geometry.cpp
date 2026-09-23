#include "geometry.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace atrium::geometry {

wlr_box place(int w, int h, const wlr_box& area, const wlr_box* parent,
              std::span<const wlr_box> others, int step) {
    wlr_box g{0, 0, std::min(w, area.width), std::min(h, area.height)};

    if (parent) {
        g.x = parent->x + (parent->width - g.width) / 2;
        g.y = parent->y + (parent->height - g.height) / 2;
    } else {
        g.x = area.x + (area.width - g.width) / 2;
        g.y = area.y + (area.height - g.height) / 2;
        const size_t limit = others.size() + 1;
        for (size_t tries = 0; tries < limit && step > 0; ++tries) {
            const bool taken = std::ranges::any_of(others, [&](const wlr_box& o) {
                return std::abs(o.x - g.x) < step && std::abs(o.y - g.y) < step;
            });
            if (!taken)
                break;
            g.x += step;
            g.y += step;
            if (g.x + g.width > area.x + area.width || g.y + g.height > area.y + area.height) {
                g.x = area.x;
                g.y = area.y;
            }
        }
    }

    g.x = std::clamp(g.x, area.x, std::max(area.x, area.x + area.width - g.width));
    g.y = std::clamp(g.y, area.y, std::max(area.y, area.y + area.height - g.height));
    return g;
}

void snap(int& x, int& y, int w, int h, const wlr_box& area, int distance) {
    if (std::abs(x - area.x) < distance)
        x = area.x;
    else if (std::abs(x + w - (area.x + area.width)) < distance)
        x = area.x + area.width - w;
    if (std::abs(y - area.y) < distance)
        y = area.y;
    else if (std::abs(y + h - (area.y + area.height)) < distance)
        y = area.y + area.height - h;
}

wlr_box resize(const wlr_box& start, uint32_t edges, int dx, int dy) {
    wlr_box b = start;
    if (edges & WLR_EDGE_LEFT) {
        b.width = std::max(start.width - dx, 1);
        b.x = start.x + start.width - b.width;
    } else if (edges & WLR_EDGE_RIGHT) {
        b.width = std::max(start.width + dx, 1);
    }
    if (edges & WLR_EDGE_TOP) {
        b.height = std::max(start.height - dy, 1);
        b.y = start.y + start.height - b.height;
    } else if (edges & WLR_EDGE_BOTTOM) {
        b.height = std::max(start.height + dy, 1);
    }
    return b;
}

wlr_box clamp_to_hints(wlr_box box, const wlr_box& min, const wlr_box& max) {
    box.width = std::max({box.width, min.width, 1});
    box.height = std::max({box.height, min.height, 1});
    if (max.width > 0)
        box.width = std::min(box.width, std::max(max.width, 1));
    if (max.height > 0)
        box.height = std::min(box.height, std::max(max.height, 1));
    return box;
}

uint32_t nearest_corner(const wlr_box& window, double cx, double cy) {
    return (cx < window.x + window.width / 2.0 ? WLR_EDGE_LEFT : WLR_EDGE_RIGHT) |
           (cy < window.y + window.height / 2.0 ? WLR_EDGE_TOP : WLR_EDGE_BOTTOM);
}

uint32_t snap_zone(const wlr_box& area, double cx, double cy, int edge, int corner) {
    const bool left = cx < area.x + edge;
    const bool right = cx >= area.x + area.width - edge;
    const bool top = cy < area.y + edge;
    const bool bottom = cy >= area.y + area.height - edge;
    const bool near_top = cy < area.y + corner;
    const bool near_bottom = cy >= area.y + area.height - corner;
    const bool near_left = cx < area.x + corner;
    const bool near_right = cx >= area.x + area.width - corner;

    if (left || right) {
        const uint32_t side = left ? WLR_EDGE_LEFT : WLR_EDGE_RIGHT;
        if (near_top)
            return side | WLR_EDGE_TOP;
        if (near_bottom)
            return side | WLR_EDGE_BOTTOM;
        return side;
    }
    if (top) {
        if (near_left)
            return WLR_EDGE_LEFT | WLR_EDGE_TOP;
        if (near_right)
            return WLR_EDGE_RIGHT | WLR_EDGE_TOP;
        return WLR_EDGE_TOP;
    }
    if (bottom) {
        if (near_left)
            return WLR_EDGE_LEFT | WLR_EDGE_BOTTOM;
        if (near_right)
            return WLR_EDGE_RIGHT | WLR_EDGE_BOTTOM;
    }
    return 0;
}

wlr_box snap_box(const wlr_box& area, uint32_t zone, int gap) {
    if (zone == WLR_EDGE_TOP || !zone)
        return area;
    wlr_box inner{area.x + gap, area.y + gap, area.width - 2 * gap, area.height - 2 * gap};
    wlr_box b = inner;
    const int half_w = (inner.width - gap) / 2;
    const int half_h = (inner.height - gap) / 2;
    if (zone & (WLR_EDGE_LEFT | WLR_EDGE_RIGHT)) {
        b.width = half_w;
        if (zone & WLR_EDGE_RIGHT)
            b.x = inner.x + inner.width - half_w;
    }
    if ((zone & (WLR_EDGE_LEFT | WLR_EDGE_RIGHT)) && (zone & (WLR_EDGE_TOP | WLR_EDGE_BOTTOM))) {
        b.height = half_h;
        if (zone & WLR_EDGE_BOTTOM)
            b.y = inner.y + inner.height - half_h;
    }
    return b;
}

wlr_box fit_into(wlr_box box, const wlr_box& area) {
    box.width = std::clamp(box.width, 1, std::max(1, area.width));
    box.height = std::clamp(box.height, 1, std::max(1, area.height));
    box.x = std::clamp(box.x, area.x, area.x + area.width - box.width);
    box.y = std::clamp(box.y, area.y, area.y + area.height - box.height);
    return box;
}

namespace {

struct Candidate {
    std::vector<wlr_box> boxes;
    double coverage = -1;  // total scaled window area
};

// Lay `order` out in `rows` rows of about equal count.
Candidate try_rows(std::span<const wlr_box> windows, const std::vector<size_t>& order,
                   const wlr_box& area, int gap, int label, int rows) {
    const size_t n = order.size();
    std::vector<std::vector<size_t>> grid(static_cast<size_t>(rows));
    for (size_t i = 0; i < n; ++i)
        grid[i * size_t(rows) / n].push_back(order[i]);
    // Inside a row, left to right.
    for (auto& row : grid)
        std::ranges::sort(row, [&](size_t a, size_t b) {
            return windows[a].x * 2 + windows[a].width < windows[b].x * 2 + windows[b].width;
        });

    // One height for every row: the tallest that lets each row fit the width
    // and all rows fit the height.
    double h = double(area.height - (rows - 1) * gap - rows * label) / rows;
    for (const auto& row : grid) {
        double aspect = 0;
        for (size_t i : row)
            aspect += double(windows[i].width) / std::max(windows[i].height, 1);
        const double fit = double(area.width - int(row.size() - 1) * gap) / aspect;
        h = std::min(h, fit);
    }
    Candidate c;
    if (h <= 1)
        return c;
    c.boxes.resize(n);
    c.coverage = 0;

    // Windows shorter than h keep their size; everything is centered.
    const double used_h = rows * (h + label) + (rows - 1) * gap;
    double y = area.y + (area.height - used_h) / 2;
    for (const auto& row : grid) {
        std::vector<wlr_box> scaled;
        double row_w = double(row.size() - 1) * gap;
        for (size_t i : row) {
            const wlr_box& w = windows[i];
            const double s = std::min(1.0, h / std::max(w.height, 1));
            scaled.push_back({0, 0, std::max(1, int(std::lround(w.width * s))),
                              std::max(1, int(std::lround(w.height * s)))});
            row_w += scaled.back().width;
        }
        double x = area.x + (area.width - row_w) / 2;
        for (size_t k = 0; k < row.size(); ++k) {
            wlr_box b = scaled[k];
            b.x = int(std::lround(x));
            b.y = int(std::lround(y + (h - b.height) / 2));
            c.boxes[row[k]] = b;
            c.coverage += double(b.width) * b.height;
            x += b.width + gap;
        }
        y += h + label + gap;
    }
    return c;
}

} // namespace

std::vector<wlr_box> overview_layout(std::span<const wlr_box> windows, const wlr_box& area,
                                     int gap, int label) {
    const size_t n = windows.size();
    if (!n)
        return {};
    // Top to bottom by center, so rows keep the windows' vertical order.
    std::vector<size_t> order(n);
    for (size_t i = 0; i < n; ++i)
        order[i] = i;
    std::ranges::stable_sort(order, [&](size_t a, size_t b) {
        return windows[a].y * 2 + windows[a].height < windows[b].y * 2 + windows[b].height;
    });

    Candidate best;
    for (int rows = 1; rows <= int(n); ++rows) {
        Candidate c = try_rows(windows, order, area, gap, label, rows);
        // A new row has to earn its place: a small gain is not worth
        // shrinking every window.
        if (c.coverage > best.coverage * 1.05)
            best = std::move(c);
    }
    if (best.boxes.empty())  // area too small for anything: stack them in the middle
        for (size_t i = 0; i < n; ++i)
            best.boxes.push_back({area.x + area.width / 2, area.y + area.height / 2, 1, 1});
    return best.boxes;
}

} // namespace atrium::geometry
