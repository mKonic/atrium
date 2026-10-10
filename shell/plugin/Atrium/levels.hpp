#pragma once
// Stepping a level (volume, brightness) the way macOS does: 16 steps, or
// quarter steps for fine control, always landing on the step grid.

#include <cstdint>

namespace atrium::levels {

constexpr int kSteps = 16;
constexpr int kFineSteps = 64;

// `value` (0..1) one step up (direction > 0) or down, snapped to a grid of
// `steps`, clamped to 0..1.
double step(double value, int direction, int steps);

// A device's volume as the shell shows it, 0..1 at the device's own 0 dB:
// PulseAudio's base volume, where it has one below 100% (a headset whose
// mixer reports gain above 0 dB, which only overdrives its amplifier), else
// 100%. `norm` is PA_VOLUME_NORM; `base` 0 when the device has none.
double shown_volume(uint32_t raw, uint32_t base, uint32_t norm);
// The raw volume for a shown one, never past that 0 dB.
uint32_t raw_volume(double shown, uint32_t base, uint32_t norm);

} // namespace atrium::levels
