#include "geometry.hpp"

#include <gtest/gtest.h>

#include <vector>

using namespace atrium::geometry;

namespace {

bool operator_eq(const wlr_box& a, const wlr_box& b) {
    return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
}
#define EXPECT_BOX(a, ...) EXPECT_PRED2(operator_eq, (a), (wlr_box{__VA_ARGS__}))

const wlr_box kArea{0, 0, 1000, 800};

} // namespace

TEST(Place, CentersInArea) {
    EXPECT_BOX(place(400, 300, kArea, nullptr, {}, 28), 300, 250, 400, 300);
}

TEST(Place, CentersInOffsetArea) {
    // A 32px bar on top shifts the usable area.
    EXPECT_BOX(place(400, 300, wlr_box{0, 32, 1000, 768}, nullptr, {}, 28), 300, 266, 400, 300);
}

TEST(Place, CascadesOverAnExactlyCoveredWindow) {
    std::vector<wlr_box> others{{300, 250, 400, 300}};
    EXPECT_BOX(place(400, 300, kArea, nullptr, others, 28), 328, 278, 400, 300);
}

TEST(Place, CascadesOverANearlyCoveredWindow) {
    std::vector<wlr_box> others{{300, 234, 400, 300}};  // 16px off: its title bar would hide
    EXPECT_BOX(place(400, 300, kArea, nullptr, others, 28), 328, 278, 400, 300);
}

TEST(Place, KeepsCascadingPastSeveralWindows) {
    std::vector<wlr_box> others{{300, 250, 1, 1}, {328, 278, 1, 1}};
    EXPECT_BOX(place(400, 300, kArea, nullptr, others, 28), 356, 306, 400, 300);
}

TEST(Place, LeavesDistantWindowsAlone) {
    std::vector<wlr_box> others{{0, 0, 400, 300}};
    EXPECT_BOX(place(400, 300, kArea, nullptr, others, 28), 300, 250, 400, 300);
}

TEST(Place, WrapsToTopLeftWhenCascadeLeavesTheArea) {
    // Nine windows cascaded from the center; the tenth would run off the
    // bottom (502 + 300 > 800), so it starts over at the top-left.
    std::vector<wlr_box> others;
    for (int k = 0; k < 9; ++k)
        others.push_back({300 + 28 * k, 250 + 28 * k, 400, 300});
    EXPECT_BOX(place(400, 300, kArea, nullptr, others, 28), 0, 0, 400, 300);
}

TEST(Place, CentersOverParent) {
    wlr_box parent{100, 100, 600, 400};
    EXPECT_BOX(place(200, 100, kArea, &parent, {}, 28), 300, 250, 200, 100);
}

TEST(Place, ParentPlacementStillFitsTheArea) {
    wlr_box parent{-300, -300, 400, 400};
    EXPECT_BOX(place(200, 100, kArea, &parent, {}, 28), 0, 0, 200, 100);
}

TEST(Place, ShrinksOversizedWindowsToTheArea) {
    EXPECT_BOX(place(2000, 2000, wlr_box{0, 32, 1000, 768}, nullptr, {}, 28), 0, 32, 1000, 768);
}

TEST(Snap, SticksToLeftAndTop) {
    int x = 10, y = 5;
    snap(x, y, 200, 100, kArea, 16);
    EXPECT_EQ(x, 0);
    EXPECT_EQ(y, 0);
}

TEST(Snap, SticksToRightAndBottom) {
    int x = 790, y = 695;
    snap(x, y, 200, 100, kArea, 16);
    EXPECT_EQ(x, 800);
    EXPECT_EQ(y, 700);
}

TEST(Snap, IgnoresEdgesOutOfRange) {
    int x = 400, y = 300;
    snap(x, y, 200, 100, kArea, 16);
    EXPECT_EQ(x, 400);
    EXPECT_EQ(y, 300);
}

TEST(Snap, SticksWhenDraggedPastTheEdge) {
    int x = -10, y = 0;
    snap(x, y, 200, 100, kArea, 16);
    EXPECT_EQ(x, 0);
}

TEST(Resize, BottomRightGrows) {
    EXPECT_BOX(resize({100, 100, 400, 300}, WLR_EDGE_RIGHT | WLR_EDGE_BOTTOM, 50, 20), 100, 100, 450, 320);
}

TEST(Resize, TopLeftKeepsTheOppositeCorner) {
    EXPECT_BOX(resize({100, 100, 400, 300}, WLR_EDGE_LEFT | WLR_EDGE_TOP, -50, -20), 50, 80, 450, 320);
    EXPECT_BOX(resize({100, 100, 400, 300}, WLR_EDGE_LEFT | WLR_EDGE_TOP, 50, 20), 150, 120, 350, 280);
}

TEST(Resize, NeverCollapsesAndNeverMovesTheAnchoredEdge) {
    // Dragging the left edge past the right one: 1px wide, right edge stays at 500.
    EXPECT_BOX(resize({100, 100, 400, 300}, WLR_EDGE_LEFT, 1000, 0), 499, 100, 1, 300);
    EXPECT_BOX(resize({100, 100, 400, 300}, WLR_EDGE_BOTTOM, 0, -1000), 100, 100, 400, 1);
}

TEST(Resize, SingleEdgeLeavesTheOtherAxis) {
    EXPECT_BOX(resize({100, 100, 400, 300}, WLR_EDGE_RIGHT, 30, 999), 100, 100, 430, 300);
}

TEST(ClampToHints, EnforcesMinimum) {
    EXPECT_BOX(clamp_to_hints({0, 0, 10, 10}, {0, 0, 200, 100}, {}), 0, 0, 200, 100);
}

TEST(ClampToHints, ZeroMaximumMeansUnlimited) {
    EXPECT_BOX(clamp_to_hints({0, 0, 5000, 5000}, {}, {0, 0, 0, 0}), 0, 0, 5000, 5000);
}

TEST(ClampToHints, EnforcesMaximum) {
    EXPECT_BOX(clamp_to_hints({0, 0, 5000, 5000}, {}, {0, 0, 800, 600}), 0, 0, 800, 600);
}

TEST(ClampToHints, NeverZero) {
    EXPECT_BOX(clamp_to_hints({0, 0, 0, -3}, {}, {}), 0, 0, 1, 1);
}

TEST(NearestCorner, PicksTheQuarterUnderTheCursor) {
    wlr_box w{0, 0, 100, 100};
    EXPECT_EQ(nearest_corner(w, 10, 10), uint32_t(WLR_EDGE_LEFT | WLR_EDGE_TOP));
    EXPECT_EQ(nearest_corner(w, 90, 10), uint32_t(WLR_EDGE_RIGHT | WLR_EDGE_TOP));
    EXPECT_EQ(nearest_corner(w, 10, 90), uint32_t(WLR_EDGE_LEFT | WLR_EDGE_BOTTOM));
    EXPECT_EQ(nearest_corner(w, 90, 90), uint32_t(WLR_EDGE_RIGHT | WLR_EDGE_BOTTOM));
}

namespace {
constexpr uint32_t L = WLR_EDGE_LEFT, R = WLR_EDGE_RIGHT, T = WLR_EDGE_TOP, B = WLR_EDGE_BOTTOM;
const wlr_box kScreen{0, 30, 1440, 870};  // a 30px bar on top
} // namespace

TEST(SnapZone, EdgesAndCorners) {
    EXPECT_EQ(snap_zone(kScreen, 0, 400, 4, 80), L);
    EXPECT_EQ(snap_zone(kScreen, 1439, 400, 4, 80), R);
    EXPECT_EQ(snap_zone(kScreen, 700, 31, 4, 80), T);
    EXPECT_EQ(snap_zone(kScreen, 0, 50, 4, 80), L | T);
    EXPECT_EQ(snap_zone(kScreen, 1439, 880, 4, 80), R | B);
    EXPECT_EQ(snap_zone(kScreen, 40, 31, 4, 80), L | T);
    EXPECT_EQ(snap_zone(kScreen, 700, 899, 4, 80), 0u);  // bottom edge alone does nothing
    EXPECT_EQ(snap_zone(kScreen, 700, 400, 4, 80), 0u);
    EXPECT_EQ(snap_zone(kScreen, 5, 400, 4, 80), 0u);     // not quite at the edge
}

TEST(SnapBox, HalvesQuartersAndMaximize) {
    EXPECT_BOX(snap_box(kScreen, T, 8), 0, 30, 1440, 870);
    EXPECT_BOX(snap_box(kScreen, L, 8), 8, 38, 708, 854);
    EXPECT_BOX(snap_box(kScreen, R, 8), 724, 38, 708, 854);
    EXPECT_BOX(snap_box(kScreen, R | B, 8), 724, 469, 708, 423);
    EXPECT_BOX(snap_box(kScreen, L | T, 8), 8, 38, 708, 423);
}

TEST(SnapBox, HalvesMeetWithOneGapBetween) {
    const wlr_box l = snap_box(kScreen, L, 8), r = snap_box(kScreen, R, 8);
    EXPECT_EQ(r.x - (l.x + l.width), 8);
    EXPECT_EQ(kScreen.x + kScreen.width - (r.x + r.width), 8);
    const wlr_box noGap = snap_box(kScreen, L, 0);
    EXPECT_EQ(noGap.width * 2, kScreen.width);
}

namespace {

bool overlaps(const wlr_box& a, const wlr_box& b) {
    return a.x < b.x + b.width && b.x < a.x + a.width && a.y < b.y + b.height && b.y < a.y + a.height;
}

bool inside(const wlr_box& a, const wlr_box& area) {
    return a.x >= area.x && a.y >= area.y && a.x + a.width <= area.x + area.width &&
           a.y + a.height <= area.y + area.height;
}

} // namespace

TEST(OverviewLayout, FitsWithoutOverlapAndKeepsAspect) {
    const wlr_box area{40, 80, 1360, 760};
    std::vector<wlr_box> wins;
    for (int i = 0; i < 7; ++i)
        wins.push_back({100 + i * 90, 60 + i * 50, 800 + i * 40, 500 + (i % 3) * 120});
    const auto out = overview_layout(wins, area, 24, 20);
    ASSERT_EQ(out.size(), wins.size());
    for (size_t i = 0; i < out.size(); ++i) {
        EXPECT_TRUE(inside(out[i], area)) << i;
        EXPECT_NEAR(double(out[i].width) / out[i].height, double(wins[i].width) / wins[i].height, 0.02) << i;
        EXPECT_LE(out[i].width, wins[i].width);
        for (size_t j = i + 1; j < out.size(); ++j)
            EXPECT_FALSE(overlaps(out[i], out[j])) << i << " " << j;
    }
}

TEST(OverviewLayout, NeverEnlarges) {
    const std::vector<wlr_box> wins{{0, 0, 300, 200}, {400, 0, 200, 300}};
    const auto out = overview_layout(wins, {0, 0, 1440, 900}, 24, 20);
    EXPECT_EQ(out[0].width, 300);
    EXPECT_EQ(out[1].height, 300);
    EXPECT_EQ(out[0].y + out[0].height / 2, out[1].y + out[1].height / 2);  // one row, centered
}

TEST(OverviewLayout, KeepsArrangement) {
    // Two on top, two below, big enough to need two rows.
    const std::vector<wlr_box> wins{{800, 0, 700, 450}, {0, 0, 700, 450}, {0, 500, 700, 450}, {800, 500, 700, 450}};
    const auto out = overview_layout(wins, {0, 0, 1440, 900}, 24, 20);
    EXPECT_LT(out[1].x, out[0].x);
    EXPECT_EQ(out[0].y, out[1].y);
    EXPECT_LT(out[0].y, out[2].y);
    EXPECT_LT(out[2].x, out[3].x);
}

TEST(OverviewLayout, EmptyAndTiny) {
    EXPECT_TRUE(overview_layout({}, {0, 0, 100, 100}, 8, 8).empty());
    const std::vector<wlr_box> one{{0, 0, 500, 500}};
    EXPECT_EQ(overview_layout(one, {0, 0, 4, 4}, 8, 8).size(), 1u);
}

TEST(FitInto, KeepsWhatFits) {
    EXPECT_BOX(fit_into({100, 100, 400, 300}, kScreen), 100, 100, 400, 300);
}

TEST(FitInto, PullsBackAndShrinks) {
    EXPECT_BOX(fit_into({1300, 800, 400, 300}, kScreen), 1040, 600, 400, 300);  // off the bottom right
    EXPECT_BOX(fit_into({-50, 0, 400, 300}, kScreen), 0, 30, 400, 300);         // above the bar
    EXPECT_BOX(fit_into({0, 0, 3000, 2000}, kScreen), 0, 30, 1440, 870);       // bigger than the screen
}

TEST(Geometry, SecretFrameLeavesAMargin) {
    const wlr_box o{1920, 0, 1920, 1080};
    const wlr_box f = atrium::geometry::secret_frame(o, 5);
    EXPECT_EQ(f.x, 1920 + 96);
    EXPECT_EQ(f.y, 54);
    EXPECT_EQ(f.width, 1920 - 192);
    EXPECT_EQ(f.height, 1080 - 108);
    const wlr_box full = atrium::geometry::secret_frame(o, 0);
    EXPECT_EQ(full.width, 1920);
    EXPECT_EQ(atrium::geometry::secret_frame(o, 90).width, 1920 - 2 * 768);  // clamped to 40%
}

TEST(Dwindle, OneWindowTakesTheWholeArea) {
    const auto boxes = atrium::geometry::dwindle(1, {100, 50, 1000, 600}, 10);
    ASSERT_EQ(boxes.size(), 1u);
    EXPECT_EQ(boxes[0].x, 100);
    EXPECT_EQ(boxes[0].width, 1000);
    EXPECT_EQ(boxes[0].height, 600);
}

TEST(Dwindle, EachNewWindowHalvesTheLastAlongItsLongerSide) {
    const wlr_box area{0, 0, 1010, 600};
    const auto b = atrium::geometry::dwindle(3, area, 10);
    ASSERT_EQ(b.size(), 3u);
    // Side by side first (the area is wide)...
    EXPECT_EQ(b[0].x, 0);
    EXPECT_EQ(b[0].width, 500);
    EXPECT_EQ(b[0].height, 600);
    // ...then the right half (tall) splits top and bottom.
    EXPECT_EQ(b[1].x, 510);
    EXPECT_EQ(b[1].width, 500);
    EXPECT_EQ(b[1].height, 295);
    EXPECT_EQ(b[2].y, 305);
    EXPECT_EQ(b[2].height, 295);
    // Nothing overlaps, and the gaps are exact.
    EXPECT_EQ(b[1].x - (b[0].x + b[0].width), 10);
    EXPECT_EQ(b[2].y - (b[1].y + b[1].height), 10);
}

TEST(Dwindle, NoWindowsNoBoxes) {
    EXPECT_TRUE(atrium::geometry::dwindle(0, {0, 0, 100, 100}, 5).empty());
}
