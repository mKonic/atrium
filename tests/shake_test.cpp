#include "shake.hpp"

#include <gtest/gtest.h>

using atrium::ShakeDetector;

namespace {

// Back and forth `swings` times over `width` px, one point every 10 ms.
bool shake(ShakeDetector& d, int swings, double width, uint32_t& ms, double y = 300) {
    bool seen = false;
    for (int s = 0; s < swings; ++s)
        for (int i = 0; i <= 5; ++i) {
            const double t = i / 5.0;
            const double x = 500 + (s % 2 ? width * (1 - t) : width * t);
            seen = d.feed(ms += 10, x, y) || seen;
        }
    return seen;
}

} // namespace

TEST(Shake, QuickBackAndForthIsAShake) {
    ShakeDetector d;
    uint32_t ms = 1000;
    EXPECT_TRUE(shake(d, 8, 150, ms));
}

TEST(Shake, OneLongSweepIsNot) {
    ShakeDetector d;
    uint32_t ms = 1000;
    bool seen = false;
    for (int i = 0; i < 60; ++i)
        seen = d.feed(ms += 10, 100 + i * 20, 300) || seen;
    EXPECT_FALSE(seen);
}

TEST(Shake, TwoSwingsAreNot) {
    ShakeDetector d;
    uint32_t ms = 1000;
    EXPECT_FALSE(shake(d, 2, 150, ms));
}

TEST(Shake, JitterInPlaceIsNot) {
    // Tiny movements (a hand resting on the mouse) cover too little ground.
    ShakeDetector d;
    uint32_t ms = 1000;
    EXPECT_FALSE(shake(d, 12, 20, ms));
}

TEST(Shake, SlowSwingsAreNot) {
    // The same swings spread over seconds: never enough of them in the window.
    ShakeDetector d;
    uint32_t ms = 1000;
    bool seen = false;
    for (int s = 0; s < 8; ++s)
        for (int i = 0; i <= 5; ++i) {
            const double t = i / 5.0;
            seen = d.feed(ms += 100, 500 + (s % 2 ? 150 * (1 - t) : 150 * t), 300) || seen;
        }
    EXPECT_FALSE(seen);
}

TEST(Shake, APauseStartsOver) {
    ShakeDetector d;
    uint32_t ms = 1000;
    EXPECT_FALSE(shake(d, 3, 150, ms));
    ms += 1000;
    EXPECT_FALSE(shake(d, 3, 150, ms));
}
