#include "hot_corners_core.hpp"

#include <cmath>

namespace atrium::hot_corners {

std::optional<Corner> corner_at(const Rect& s, double x, double y) {
    const int px = int(std::floor(x)), py = int(std::floor(y));
    const bool left = px == s.x, right = px == s.x + s.width - 1;
    const bool top = py == s.y, bottom = py == s.y + s.height - 1;
    if (top && left)
        return Corner::TopLeft;
    if (top && right)
        return Corner::TopRight;
    if (bottom && left)
        return Corner::BottomLeft;
    if (bottom && right)
        return Corner::BottomRight;
    return std::nullopt;
}

void push_back(Corner c, double& x, double& y) {
    x += (c == Corner::TopLeft || c == Corner::BottomLeft) ? kPushBack : -kPushBack;
    y += (c == Corner::TopLeft || c == Corner::TopRight) ? kPushBack : -kPushBack;
}

bool Edge::far_from_last(double x, double y) const {
    return std::abs(x - tx_) + std::abs(y - ty_) > kResetDistance;
}

Result Edge::check(bool at, double x, double y, double now) {
    if (!at) {
        if (far_from_last(x, y))
            last_reset_.reset();
        return Result::Nothing;
    }
    if (last_trigger_ && now - *last_trigger_ < kCooldownMs - kDelayMs) {
        // Still resting: it has to be held still this long to fire again.
        last_trigger_ = now;
        return Result::Nothing;
    }
    if (can_activate(x, y, now)) {
        last_trigger_ = now;
        last_reset_.reset();
        tx_ = x;
        ty_ = y;
        return Result::Trigger;
    }
    tx_ = x;
    ty_ = y;
    return Result::PushBack;
}

bool Edge::can_activate(double x, double y, double now) {
    // The first touch of a new attempt.
    if (!last_reset_ || now - *last_reset_ > kCooldownMs) {
        last_reset_ = now;
        return false;
    }
    if (last_trigger_ && now - *last_trigger_ < kCooldownMs - kDelayMs)
        return false;
    if (now - *last_reset_ < kDelayMs)
        return false;
    return !far_from_last(x, y);
}

namespace {

// main.js numbers the corners clockwise from the top left.
constexpr Corner kClockwise[4] = {Corner::TopLeft, Corner::TopRight, Corner::BottomRight, Corner::BottomLeft};

} // namespace

std::vector<Corner> aperture_corners(const Rect& s, const std::vector<Rect>& windows) {
    const size_t n = windows.size();
    std::vector<std::array<double, 4>> distances(n);
    std::vector<int> corner(n, -1);
    std::vector<bool> unplaced(n, true);
    int closest[4] = {-1, -1, -1, -1};
    for (size_t i = 0; i < n; i++) {
        const Rect& g = windows[i];
        const double dl = g.x + g.width - s.x, dr = s.x + s.width - g.x;
        const double dt = g.y + g.height - s.y, db = s.y + s.height - g.y;
        distances[i] = {dl + dt, dr + dt, dr + db, dl + db};
        int nearest = 0;
        for (int j = 1; j < 4; j++)
            if (distances[i][j] < distances[i][nearest] ||
                (distances[i][j] == distances[i][nearest] && closest[j] < 0))
                nearest = j;
        if (closest[nearest] < 0 || distances[closest[nearest]][nearest] > distances[i][nearest])
            closest[nearest] = int(i);
    }
    // The nearest windows to their nearest corners first, so a lone window
    // at the bottom right isn't sent off past the top left.
    int taken[4] = {0, 0, 0, 0};
    for (int c = 0; c < 4; c++) {
        if (closest[c] < 0)
            continue;
        corner[size_t(closest[c])] = c;
        unplaced[size_t(closest[c])] = false;
        taken[c] = 1;
    }
    // Then the rest, a quarter to each corner, by preference.
    const int per_corner = int((n + 3) / 4);
    for (int c = 0; c < 4; c++) {
        for (int k = 0; k < per_corner - taken[c]; k++) {
            int best = -1;
            for (size_t i = 0; i < n; i++)
                if (unplaced[i] && (best < 0 || distances[i][c] < distances[size_t(best)][c]))
                    best = int(i);
            if (best < 0)
                break;
            corner[size_t(best)] = c;
            unplaced[size_t(best)] = false;
        }
    }
    std::vector<Corner> out(n);
    for (size_t i = 0; i < n; i++)
        out[i] = kClockwise[corner[i] < 0 ? 0 : corner[i]];
    return out;
}

void aperture_target(const Rect& s, const Rect& w, Corner c, int& x, int& y) {
    const int dx = s.width / 16, dy = s.height / 16;
    const bool right = c == Corner::TopRight || c == Corner::BottomRight;
    const bool bottom = c == Corner::BottomLeft || c == Corner::BottomRight;
    // Anchored by its left edge at the right side, its right edge at the left.
    x = right ? s.x + s.width - dx : s.x + dx - w.width;
    y = bottom ? s.y + s.height - dy : s.y + dy - w.height;
}

} // namespace atrium::hot_corners
