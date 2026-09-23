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

} // namespace atrium::geometry
