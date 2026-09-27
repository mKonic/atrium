#pragma once
// Shaking the pointer to find it (macOS, KWin's shakecursor): a quick back
// and forth whose path is much longer than the ground it covers.

#include <cstdint>
#include <deque>

namespace atrium {

class ShakeDetector {
public:
    // A pointer position at `ms`; true while the last moments were a shake.
    bool feed(uint32_t ms, double x, double y);
    void reset() { points_.clear(); }

    static constexpr uint32_t kWindowMs = 800;   // how far back a shake is judged
    static constexpr double kMinReach = 60;      // the ground it covers, at least (px)
    static constexpr double kPathRatio = 3.0;    // path length over that ground
    static constexpr int kMinReversals = 4;      // left-right (or up-down) turns

private:
    struct Point {
        uint32_t ms;
        double x, y;
    };
    std::deque<Point> points_;
};

} // namespace atrium
