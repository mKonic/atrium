#pragma once
// Hot corners, as KWin's screen edges trigger (screenedge.cpp): each
// screen's four corners (ElectricBorderAllScreenCorner), hit when the pointer
// is on the very corner pixel. The first touch pushes the pointer back a
// pixel; pushing on into the corner for 75 ms (ElectricBorderDelay) sets it
// off; then it rests for 350 ms (ElectricBorderCooldown), and only fires
// again once held still that long. Moving 30 px away starts afresh.
#include <array>
#include <optional>
#include <vector>

namespace atrium::hot_corners {

enum class Corner { TopLeft, TopRight, BottomLeft, BottomRight };
constexpr std::array<const char*, 4> kCornerKeys{"top_left", "top_right", "bottom_left", "bottom_right"};

constexpr double kDelayMs = 75;      // ElectricBorderDelay
constexpr double kCooldownMs = 350;  // ElectricBorderCooldown (at least the delay + 50)
constexpr double kResetDistance = 30;  // DISTANCE_RESET, Manhattan
constexpr int kPushBack = 1;         // ElectricBorderPushbackPixels

struct Rect {
    int x, y, width, height;
};

// Which corner of `screen` the pointer is on, by its pixel.
std::optional<Corner> corner_at(const Rect& screen, double x, double y);

// Where a pushed-back pointer goes: a pixel in from the corner.
void push_back(Corner c, double& x, double& y);

enum class Result { Nothing, PushBack, Trigger };

// One corner's state: Edge::check, canActivate and markAsTriggered.
class Edge {
public:
    // `at` is whether the pointer is on this corner now.
    Result check(bool at, double x, double y, double now_ms);

private:
    bool can_activate(double x, double y, double now_ms);
    bool far_from_last(double x, double y) const;

    std::optional<double> last_trigger_, last_reset_;
    double tx_ = 0, ty_ = 0;  // m_triggeredPoint
};

// Show Desktop, as KWin's Window Aperture (windowaperture/main.js): each
// window goes off past a corner of its screen, fading out. The window nearest
// each corner takes it, and the rest share the corners evenly by their
// distance. `windows` in stacking order, bottom first; the corner for each.
std::vector<Corner> aperture_corners(const Rect& screen, const std::vector<Rect>& windows);
// Where a window going to `c` ends (its top-left): with the corner of it
// nearest the screen's middle a sixteenth of the screen in from that corner.
void aperture_target(const Rect& screen, const Rect& window, Corner c, int& x, int& y);

} // namespace atrium::hot_corners
