#include "eis_core.hpp"

#include <algorithm>

namespace atrium::eis {

namespace {

// Whether the segment [a0, a1] of the line at `at` (x when vertical) is
// covered by an edge of `z`, and on which side the screen lies.
bool on_edge(const Zone& z, bool vertical, int at, int a0, int a1) {
    if (vertical) {
        const bool edge = at == z.x || at == z.x + z.width;
        return edge && a0 >= z.y && a1 <= z.y + z.height;
    }
    const bool edge = at == z.y || at == z.y + z.height;
    return edge && a0 >= z.x && a1 <= z.x + z.width;
}

// Whether another screen lies across `from`'s edge at `at` along [a0, a1]:
// past a left (top) edge one ends there, past a right (bottom) edge one
// starts there.
bool neighbour(const std::vector<Zone>& zones, const Zone& from, bool vertical, int at, int a0, int a1) {
    for (const Zone& z : zones) {
        if (&z == &from)
            continue;
        const int start = vertical ? z.x : z.y, size = vertical ? z.width : z.height;
        const int from_start = vertical ? from.x : from.y;
        const bool across = at == from_start ? start + size == at : start == at;
        const int z0 = vertical ? z.y : z.x, z1 = z0 + (vertical ? z.height : z.width);
        if (across && a0 < z1 && a1 > z0)
            return true;
    }
    return false;
}

} // namespace

bool valid(const Barrier& b, const std::vector<Zone>& zones) {
    const bool vertical = b.x1 == b.x2, horizontal = b.y1 == b.y2;
    if (vertical == horizontal)
        return false;  // diagonal, or a point
    const int at = vertical ? b.x1 : b.y1;
    const int a0 = vertical ? std::min(b.y1, b.y2) : std::min(b.x1, b.x2);
    const int a1 = vertical ? std::max(b.y1, b.y2) : std::max(b.x1, b.x2);
    for (const Zone& z : zones)
        if (on_edge(z, vertical, at, a0, a1) && !neighbour(zones, z, vertical, at, a0, a1))
            return true;
    return false;
}

std::optional<uint32_t> crossed(const std::vector<Barrier>& barriers, double x, double y, double dx, double dy) {
    for (const Barrier& b : barriers) {
        if (b.x1 == b.x2) {
            const double lo = std::min(b.y1, b.y2), hi = std::max(b.y1, b.y2);
            if (y < lo || y > hi || dx == 0)
                continue;
            // The pointer stops a hair inside the right and bottom edges.
            const double at = b.x1;
            if ((x < at && x + dx >= at) || (x >= at && x + dx < at))
                return b.id;
        } else {
            const double lo = std::min(b.x1, b.x2), hi = std::max(b.x1, b.x2);
            if (x < lo || x > hi || dy == 0)
                continue;
            const double at = b.y1;
            if ((y < at && y + dy >= at) || (y >= at && y + dy < at))
                return b.id;
        }
    }
    return std::nullopt;
}

} // namespace atrium::eis
