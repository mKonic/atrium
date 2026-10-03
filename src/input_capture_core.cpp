#include "input_capture_core.hpp"

#include <algorithm>

namespace atrium::input_capture {

namespace {

bool contains(const Box& b, int x, int y) {
    return x >= b.x && x < b.x + b.width && y >= b.y && y < b.y + b.height;
}

// Any screen covering pixels of column `x` (or row `y`) between `from` and `to`.
bool covered_column(std::span<const Box> screens, int x, int from, int to) {
    for (const Box& s : screens)
        if (x >= s.x && x < s.x + s.width && s.y <= to && s.y + s.height - 1 >= from)
            return true;
    return false;
}

bool covered_row(std::span<const Box> screens, int y, int from, int to) {
    for (const Box& s : screens)
        if (y >= s.y && y < s.y + s.height && s.x <= to && s.x + s.width - 1 >= from)
            return true;
    return false;
}

} // namespace

bool valid(const Barrier& b, std::span<const Box> screens) {
    if (b.id == 0)
        return false;
    if (b.x1 == b.x2) {
        const int x = b.x1, from = std::min(b.y1, b.y2), to = std::max(b.y1, b.y2);
        for (const Box& s : screens) {
            if (from < s.y || to > s.y + s.height - 1)
                continue;  // not all on this screen
            if (x == s.x && !covered_column(screens, x - 1, from, to))
                return true;  // its left edge, nothing to its left
            if (x == s.x + s.width && !covered_column(screens, x, from, to))
                return true;  // its right edge, nothing to its right
        }
        return false;
    }
    if (b.y1 == b.y2) {
        const int y = b.y1, from = std::min(b.x1, b.x2), to = std::max(b.x1, b.x2);
        for (const Box& s : screens) {
            if (from < s.x || to > s.x + s.width - 1)
                continue;
            if (y == s.y && !covered_row(screens, y - 1, from, to))
                return true;
            if (y == s.y + s.height && !covered_row(screens, y, from, to))
                return true;
        }
        return false;
    }
    return false;  // diagonal
}

std::optional<Crossing> crossing(std::span<const Barrier> barriers, double x0, double y0, double dx, double dy) {
    const double x1 = x0 + dx, y1 = y0 + dy;
    for (const Barrier& b : barriers) {
        if (b.x1 == b.x2 && dx != 0) {
            const double x = b.x1;
            // Through the line, either way (beyond it there's no screen).
            if (!((x0 < x && x1 >= x) || (x0 >= x && x1 < x)))
                continue;
            const double y = y0 + (x - x0) / dx * dy;
            if (y >= std::min(b.y1, b.y2) && y < std::max(b.y1, b.y2) + 1)
                return Crossing{b.id, x1, y1};
        } else if (b.y1 == b.y2 && dy != 0) {
            const double y = b.y1;
            if (!((y0 < y && y1 >= y) || (y0 >= y && y1 < y)))
                continue;
            const double x = x0 + (y - y0) / dy * dx;
            if (x >= std::min(b.x1, b.x2) && x < std::max(b.x1, b.x2) + 1)
                return Crossing{b.id, x1, y1};
        }
    }
    return std::nullopt;
}

} // namespace atrium::input_capture
