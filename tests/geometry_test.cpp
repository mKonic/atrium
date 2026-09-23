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
