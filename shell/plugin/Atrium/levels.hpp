#pragma once
// Stepping a level (volume, brightness) the way macOS does: 16 steps, or
// quarter steps for fine control, always landing on the step grid.

namespace atrium::levels {

constexpr int kSteps = 16;
constexpr int kFineSteps = 64;

// `value` (0..1) one step up (direction > 0) or down, snapped to a grid of
// `steps`, clamped to 0..1.
double step(double value, int direction, int steps);

} // namespace atrium::levels
