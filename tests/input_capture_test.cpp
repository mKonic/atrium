#include "input_capture_core.hpp"

#include <gtest/gtest.h>

using namespace atrium;
using namespace atrium::input_capture;

namespace {
// Two 1920x1080 screens side by side, as the portal's own example.
const Box kScreens[] = {{0, 0, 1920, 1080}, {1920, 0, 1920, 1080}};
}

TEST(InputCapture, BarriersOnTheOutsideOnly) {
    // The portal's permitted examples.
    EXPECT_TRUE(valid({1, 0, 0, 1919, 0}, kScreens));
    EXPECT_TRUE(valid({2, 0, 1080, 1919, 1080}, kScreens));
    EXPECT_TRUE(valid({3, 1920, 0, 3839, 0}, kScreens));
    EXPECT_TRUE(valid({4, 0, 0, 0, 1079}, kScreens));
    EXPECT_TRUE(valid({5, 3840, 0, 3840, 1079}, kScreens));
    // Between the screens, across both, off a screen, diagonal, or id 0.
    EXPECT_FALSE(valid({6, 1920, 0, 1920, 1079}, kScreens));
    EXPECT_FALSE(valid({7, 0, 0, 3839, 0}, kScreens));
    EXPECT_FALSE(valid({8, 0, 0, 0, 1080}, kScreens));
    EXPECT_FALSE(valid({9, 0, 0, 10, 10}, kScreens));
    EXPECT_FALSE(valid({0, 0, 0, 0, 1079}, kScreens));
    // A screen lower on the right: the left one's right edge is outside
    // only where the other isn't beside it.
    const Box stepped[] = {{0, 0, 1920, 1080}, {1920, 500, 1920, 1080}};
    EXPECT_TRUE(valid({10, 1920, 0, 1920, 499}, stepped));
    EXPECT_FALSE(valid({11, 1920, 0, 1920, 500}, stepped));
}

TEST(InputCapture, CrossingsAreFound) {
    const Barrier right{5, 3840, 0, 3840, 1079}, top{3, 1920, 0, 3839, 0};
    const Barrier both[] = {right, top};
    // Pushed past the right edge: where it would be.
    const auto c = crossing(both, 3839.5, 500, 12, 2);
    ASSERT_TRUE(c);
    EXPECT_EQ(c->id, 5u);
    EXPECT_DOUBLE_EQ(c->x, 3851.5);
    EXPECT_DOUBLE_EQ(c->y, 502);
    // Up through the top of the right screen only.
    EXPECT_EQ(crossing(both, 2000, 0.4, 0, -1)->id, 3u);
    EXPECT_FALSE(crossing(both, 1000, 0.4, 0, -1));  // the left screen's top has no barrier
    // Moving along the edge, or short of it.
    EXPECT_FALSE(crossing(both, 3839.5, 500, 0, 30));
    EXPECT_FALSE(crossing(both, 3800, 500, 20, 0));
    // Past the barrier's end.
    const Barrier part[] = {{1, 3840, 0, 3840, 99}};
    EXPECT_TRUE(crossing(part, 3839, 99.5, 2, 0));
    EXPECT_FALSE(crossing(part, 3839, 100, 2, 0));
}
