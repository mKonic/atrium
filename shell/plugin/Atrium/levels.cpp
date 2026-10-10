#include "levels.hpp"

#include <algorithm>
#include <cmath>

namespace atrium::levels {

double step(double value, int direction, int steps) {
    // A level just off the grid (a slider, rounding) steps to the next line,
    // not past it.
    constexpr double kSlack = 1e-3;
    const double pos = std::clamp(value, 0.0, 1.0) * steps;
    const double next = direction > 0 ? std::floor(pos + kSlack) + 1 : std::ceil(pos - kSlack) - 1;
    return std::clamp(next / steps, 0.0, 1.0);
}

namespace {

uint32_t full_scale(uint32_t base, uint32_t norm) {
    return base > 0 && base < norm ? base : norm;
}

} // namespace

double shown_volume(uint32_t raw, uint32_t base, uint32_t norm) {
    return double(raw) / full_scale(base, norm);
}

uint32_t raw_volume(double shown, uint32_t base, uint32_t norm) {
    return uint32_t(std::lround(std::clamp(shown, 0.0, 1.0) * full_scale(base, norm)));
}

} // namespace atrium::levels
