#include "shake.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace atrium {

namespace {

// Turns along one axis, ignoring jitter smaller than a few pixels.
int reversals(const std::vector<double>& values) {
    constexpr double kJitter = 4;
    int turns = 0, dir = 0;
    double anchor = values.front();
    for (double v : values) {
        const double d = v - anchor;
        if (std::abs(d) < kJitter)
            continue;
        const int now = d > 0 ? 1 : -1;
        if (dir && now != dir)
            ++turns;
        dir = now;
        anchor = v;
    }
    return turns;
}

} // namespace

bool ShakeDetector::feed(uint32_t ms, double x, double y) {
    // A pause ends it: judge only the recent, continuous stretch.
    if (!points_.empty() && ms - points_.back().ms > kWindowMs / 2)
        points_.clear();
    points_.push_back({ms, x, y});
    while (!points_.empty() && ms - points_.front().ms > kWindowMs)
        points_.pop_front();
    if (points_.size() < 6)
        return false;

    double left = x, right = x, top = y, bottom = y, path = 0;
    for (size_t i = 0; i < points_.size(); ++i) {
        const Point& p = points_[i];
        left = std::min(left, p.x);
        right = std::max(right, p.x);
        top = std::min(top, p.y);
        bottom = std::max(bottom, p.y);
        if (i)
            path += std::hypot(p.x - points_[i - 1].x, p.y - points_[i - 1].y);
    }
    const double reach = std::hypot(right - left, bottom - top);
    if (reach < kMinReach || path < kPathRatio * reach)
        return false;
    std::vector<double> xs, ys;
    for (const Point& p : points_) {
        xs.push_back(p.x);
        ys.push_back(p.y);
    }
    const int turns = std::max(reversals(xs), reversals(ys));
    return turns >= kMinReversals;
}

} // namespace atrium
