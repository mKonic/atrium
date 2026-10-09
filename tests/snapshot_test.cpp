#include "../src/snapshot_core.hpp"

#include <gtest/gtest.h>

#include <utility>

using namespace atrium;

namespace {

TEST(Snapshot, PartsKeepTheirPlaceInTheFrame) {
    // A 100x50 frame drawn at half size, moved: the title bar's strip and
    // a shadow hanging outside the frame follow it.
    const FBox to{200, 100, 50, 25};
    EXPECT_EQ(snapshot_map({0, 0, 100, 10}, 100, 50, to), (FBox{200, 100, 50, 5}));
    EXPECT_EQ(snapshot_map({-20, -20, 140, 90}, 100, 50, to), (FBox{190, 90, 70, 45}));
}

TEST(Snapshot, ItStretchesEachWaySeparately) {
    // Maximizing: a 100x100 window into a 400x200 box.
    EXPECT_EQ(snapshot_map({50, 50, 50, 50}, 100, 100, {0, 0, 400, 200}), (FBox{200, 100, 200, 100}));
}

TEST(Snapshot, PopinShrinksAboutTheCentre) {
    EXPECT_EQ(popin_box({0, 0, 100, 60}, 0.8), (FBox{10, 6, 80, 48}));
}

TEST(Snapshot, PopinWithNoPercentageIsAFivePixelPoint) {
    // caelestia's windowsIn: Hyprland's popin, from nothing (clamped to 5x5).
    EXPECT_EQ(popin_box({100, 100, 300, 200}, 0), (FBox{247.5, 197.5, 5, 5}));
}

TEST(Snapshot, BoxesInterpolate) {
    EXPECT_EQ(lerp({0, 0, 100, 100}, {100, 50, 0, 0}, 0.5), (FBox{50, 25, 50, 50}));
    EXPECT_EQ(lerp({1, 2, 3, 4}, {5, 6, 7, 8}, 0), (FBox{1, 2, 3, 4}));
    EXPECT_EQ(lerp({1, 2, 3, 4}, {5, 6, 7, 8}, 1), (FBox{5, 6, 7, 8}));
}

TEST(Genie, AtTheStartNothingMoves) {
    // Each with its icon on that side.
    const FBox geo{400, 400, 300, 200};
    const std::pair<GenieEdge, FBox> cases[] = {
        {GenieEdge::Bottom, {500, 900, 48, 48}},
        {GenieEdge::Top, {500, 10, 48, 48}},
        {GenieEdge::Left, {10, 500, 48, 48}},
        {GenieEdge::Right, {1800, 500, 48, 48}},
    };
    for (auto [e, icon] : cases) {
        const FPoint p = genie_point(e, geo, icon, 0, 120, 80);
        EXPECT_DOUBLE_EQ(p.x, 120);
        EXPECT_DOUBLE_EQ(p.y, 80);
    }
}

TEST(Genie, AtTheEndEverythingIsAlongTheIconsEdge) {
    // A Dock along the bottom: every row has reached the icon's top and
    // narrowed to its width, at the same fraction across.
    const FBox geo{100, 100, 300, 200}, icon{500, 900, 48, 48};
    for (double qy : {0.0, 80.0, 200.0}) {
        const FPoint left = genie_point(GenieEdge::Bottom, geo, icon, 1, 0, qy);
        const FPoint right = genie_point(GenieEdge::Bottom, geo, icon, 1, 300, qy);
        EXPECT_DOUBLE_EQ(left.y, 800);
        EXPECT_DOUBLE_EQ(left.x, 400);  // the icon's left, relative to geo
        EXPECT_DOUBLE_EQ(right.x, 448);
    }
}

TEST(Genie, RowsNearerTheIconGoFirst) {
    // magiclamp.cpp by hand: a 100x100 window, an icon 400 below it,
    // halfway. The top row: quadFactor 50, offset 500 * 0.5 * 50^3/100^3.
    const FBox geo{0, 0, 100, 100}, icon{40, 500, 20, 20};
    const FPoint top = genie_point(GenieEdge::Bottom, geo, icon, 0.5, 0, 0);
    EXPECT_DOUBLE_EQ(top.y, 31.25);
    EXPECT_DOUBLE_EQ(top.x, 40 * (31.25 / 500));
    const FPoint bottom = genie_point(GenieEdge::Bottom, geo, icon, 0.5, 0, 100);
    EXPECT_GT(bottom.y - 100, top.y);
}

TEST(Genie, TheEdgeFollowsTheDock) {
    const FBox screen{0, 0, 1920, 1080};
    EXPECT_EQ(genie_edge(screen, {600, 1000, 720, 80}, {700, 1010, 48, 48}), GenieEdge::Bottom);
    EXPECT_EQ(genie_edge(screen, {600, 0, 720, 80}, {700, 10, 48, 48}), GenieEdge::Top);
    EXPECT_EQ(genie_edge(screen, {0, 200, 80, 600}, {10, 300, 48, 48}), GenieEdge::Left);
    EXPECT_EQ(genie_edge(screen, {1840, 200, 80, 600}, {1850, 300, 48, 48}), GenieEdge::Right);
}

} // namespace
