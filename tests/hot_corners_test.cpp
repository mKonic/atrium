#include "../src/hot_corners_core.hpp"

#include <gtest/gtest.h>

using namespace atrium::hot_corners;

namespace {

constexpr Rect kScreen{0, 0, 1600, 900};

} // namespace

TEST(HotCorners, OnlyTheCornerPixel) {
    EXPECT_EQ(corner_at(kScreen, 0, 0), Corner::TopLeft);
    EXPECT_EQ(corner_at(kScreen, 1599.7, 0), Corner::TopRight);
    EXPECT_EQ(corner_at(kScreen, 0, 899.2), Corner::BottomLeft);
    EXPECT_EQ(corner_at(kScreen, 1599, 899), Corner::BottomRight);
    EXPECT_FALSE(corner_at(kScreen, 1, 0));
    EXPECT_FALSE(corner_at(kScreen, 0, 1));
    EXPECT_FALSE(corner_at(kScreen, 800, 450));
    // A second screen's corners are its own.
    EXPECT_EQ(corner_at({1600, 0, 1920, 1080}, 1600, 1079), Corner::BottomLeft);
}

TEST(HotCorners, PushBackGoesInward) {
    double x = 1599, y = 0;
    push_back(Corner::TopRight, x, y);
    EXPECT_EQ(x, 1598);
    EXPECT_EQ(y, 1);
}

TEST(HotCorners, FiresAfterPushingOnForTheDelay) {
    Edge e;
    EXPECT_EQ(e.check(true, 0, 0, 1000), Result::PushBack);  // the first touch
    EXPECT_EQ(e.check(true, 0, 0, 1040), Result::PushBack);  // too soon
    EXPECT_EQ(e.check(true, 0, 0, 1080), Result::Trigger);
}

TEST(HotCorners, RestsAfterFiring) {
    Edge e;
    e.check(true, 0, 0, 0);
    ASSERT_EQ(e.check(true, 0, 0, 100), Result::Trigger);
    // Held in the corner: nothing until it has been still for the cooldown.
    EXPECT_EQ(e.check(true, 0, 0, 200), Result::Nothing);
    EXPECT_EQ(e.check(true, 0, 0, 400), Result::Nothing);
    EXPECT_EQ(e.check(true, 0, 0, 500), Result::Nothing);
    // Away and back after the rest: a new attempt.
    EXPECT_EQ(e.check(false, 100, 100, 900), Result::Nothing);
    EXPECT_EQ(e.check(true, 0, 0, 1000), Result::PushBack);
    EXPECT_EQ(e.check(true, 0, 0, 1100), Result::Trigger);
}

TEST(HotCorners, ASlowSecondTouchStartsOver) {
    Edge e;
    e.check(true, 0, 0, 0);
    // Back much later than the cooldown: this is a first touch again.
    EXPECT_EQ(e.check(true, 0, 0, 1000), Result::PushBack);
    EXPECT_EQ(e.check(true, 0, 0, 1100), Result::Trigger);
}

TEST(HotCorners, MovingFarAwayResets) {
    Edge e;
    e.check(true, 0, 0, 0);
    e.check(false, 40, 0, 50);  // 40 px off: forgotten
    EXPECT_EQ(e.check(true, 0, 0, 100), Result::PushBack);
}

TEST(Aperture, ALoneWindowGoesToItsNearestCorner) {
    const auto c = aperture_corners(kScreen, {{1000, 500, 400, 300}});
    ASSERT_EQ(c.size(), 1u);
    EXPECT_EQ(c[0], Corner::BottomRight);
}

TEST(Aperture, FourWindowsShareTheCorners) {
    const auto c = aperture_corners(kScreen, {
        {10, 10, 300, 200}, {1200, 10, 300, 200}, {1200, 600, 300, 200}, {10, 600, 300, 200}});
    EXPECT_EQ(c[0], Corner::TopLeft);
    EXPECT_EQ(c[1], Corner::TopRight);
    EXPECT_EQ(c[2], Corner::BottomRight);
    EXPECT_EQ(c[3], Corner::BottomLeft);
}

TEST(Aperture, CrowdedCornersSpillOver) {
    // Eight windows all at the top left: two per corner.
    std::vector<Rect> w(8, Rect{10, 10, 300, 200});
    const auto c = aperture_corners(kScreen, w);
    int count[4] = {};
    for (Corner k : c)
        count[int(k)]++;
    for (int n : count)
        EXPECT_EQ(n, 2);
}

TEST(Aperture, TargetLeavesAPeekPastTheCorner) {
    int x, y;
    aperture_target(kScreen, {500, 300, 400, 300}, Corner::TopLeft, x, y);
    EXPECT_EQ(x + 400, 100);  // its right edge a sixteenth in
    EXPECT_EQ(y + 300, 56);
    aperture_target(kScreen, {500, 300, 400, 300}, Corner::BottomRight, x, y);
    EXPECT_EQ(x, 1500);
    EXPECT_EQ(y, 844);
}
