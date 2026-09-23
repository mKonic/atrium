#include "geometry.hpp"

#include <algorithm>
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

} // namespace atrium::geometry
