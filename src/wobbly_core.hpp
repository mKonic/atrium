#pragma once
// Wobbly windows, as KWin's effect does them (wobblywindows.cpp): a 4x4
// grid of points on springs, one of them held by the pointer while the
// window is dragged, the rest pulled along by their neighbours, smoothed and
// damped, integrated in 10 ms steps. The window is drawn as the bicubic
// Bezier surface the grid's points control. Resizing wobbles only the sides
// that have moved.
#include "snapshot_core.hpp"

#include <array>

namespace atrium {

struct WobblyParams {
    double stiffness, drag, move_factor;
    double min_velocity, max_velocity, stop_velocity;
    double min_acceleration, max_acceleration, stop_acceleration;
};

// KWin's five Wobbliness levels, 0 (its default) to 4.
WobblyParams wobbly_preset(int level);

// KWin's tessellation of the drawn surface, each way.
constexpr int kWobblyTessellation = 20;

class Wobbly {
public:
    // A drag starting at `pointer` (layout) on a window at `rect`: that
    // grid point is held. A resize wobbles no side until it has moved.
    Wobbly(const FBox& rect, const WobblyParams& params, FPoint pointer, bool resize);

    // During and at the end of the drag or resize: which sides have moved.
    void moved(const FBox& rect);
    // The drag or resize is over: the held point lets go once it settles.
    void release(const FBox& rect);
    // Run the springs for `ms` more, the window now at `rect`; false once
    // it has settled after the release (then it's drawn flat again).
    bool advance(const FBox& rect, double ms);

    // Where the point u, v (0 to 1 across the window) is drawn, in layout
    // coordinates.
    FPoint at(double u, double v) const;
    bool wobbling() const { return wobbling_; }

private:
    static constexpr int kSide = 4, kCount = kSide * kSide;
    using Grid = std::array<FPoint, kCount>;

    bool step(const FBox& rect, double ms);
    void smooth(Grid& data);
    void sides_moved(const FBox& rect);

    WobblyParams p_;
    Grid origin_{}, position_{}, velocity_{}, acceleration_{};
    std::array<bool, kCount> held_{};
    bool moving_ = true;
    bool wobbling_ = false;
    bool top_ = true, left_ = true, right_ = true, bottom_ = true;
    FBox resize_from_{};
};

} // namespace atrium
