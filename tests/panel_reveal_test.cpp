#include "panel_edge_core.hpp"
#include "reveal_core.hpp"

#include <gtest/gtest.h>

using namespace atrium;
using panel_edge::Edge;
using panel_edge::Over;

namespace {

// A 1920x1080 screen with a 30 px menu bar and an 80 px Dock.
panel_edge::Pointer at(double y, Over over = Over::Desktop, bool blocked = false) {
    return {.screen = 0, .top = 0, .bottom = 1080, .y = y, .over = over, .blocked = blocked, .top_band = 30, .bottom_band = 80};
}

} // namespace

// --- The pointer and the edge (the compositor's half) ------------------------

TEST(PanelEdge, RestingOnTheLastRowReachesItAfterTheDwell) {
    panel_edge::Tracker t;
    EXPECT_EQ(t.update(at(1079), 0), Edge::None);
    EXPECT_DOUBLE_EQ(*t.wake_in(0), panel_edge::kDwellMs);
    EXPECT_EQ(t.update(at(1079), 199), Edge::None);
    EXPECT_EQ(t.update(at(1079), 200), Edge::Bottom);
    EXPECT_FALSE(t.wake_in(200));
}

TEST(PanelEdge, ThreePixelsInIsNotTheEdge) {
    panel_edge::Tracker t;
    t.update(at(1076), 0);
    EXPECT_FALSE(t.wake_in(0));
    EXPECT_EQ(t.update(at(1076), 1000), Edge::None);
    t.update(at(1), 2000);
    EXPECT_EQ(t.update(at(1), 3000), Edge::None);
}

TEST(PanelEdge, PassingOverTheEdgeDoesNothing) {
    panel_edge::Tracker t;
    t.update(at(1079), 0);
    EXPECT_EQ(t.update(at(900), 150), Edge::None);
    // Coming back starts the wait again.
    EXPECT_EQ(t.update(at(1079), 250), Edge::None);
    EXPECT_EQ(t.update(at(1079), 440), Edge::None);
    EXPECT_EQ(t.update(at(1079), 450), Edge::Bottom);
}

TEST(PanelEdge, OnceReachedItHoldsThroughoutThePanelsBand) {
    panel_edge::Tracker t;
    t.update(at(1079), 0);
    ASSERT_EQ(t.update(at(1079), 200), Edge::Bottom);
    EXPECT_EQ(t.update(at(1000), 300), Edge::Bottom);  // the band's first row
    EXPECT_EQ(t.update(at(999), 310), Edge::None);
    // Back inside the band is not back on the edge.
    EXPECT_EQ(t.update(at(1040), 320), Edge::None);
    EXPECT_EQ(t.update(at(1040), 2000), Edge::None);

    panel_edge::Tracker top;
    top.update(at(0), 0);
    ASSERT_EQ(top.update(at(0), 200), Edge::Top);
    EXPECT_EQ(top.update(at(29.5), 300), Edge::Top);
    EXPECT_EQ(top.update(at(30), 310), Edge::None);
}

TEST(PanelEdge, AButtonDownKeepsThePanelAwayButNotAClickOnIt) {
    panel_edge::Tracker t;
    t.update(at(1079, Over::Desktop, true), 0);
    EXPECT_EQ(t.update(at(1079, Over::Desktop, true), 1000), Edge::None);
    // Let go: it waits from then.
    EXPECT_EQ(t.update(at(1079), 1000), Edge::None);
    EXPECT_EQ(t.update(at(1079), 1200), Edge::Bottom);
    // Pressing on the panel that came is a click on it.
    EXPECT_EQ(t.update(at(1040, Over::Desktop, true), 1300), Edge::Bottom);
}

TEST(PanelEdge, FullscreenTheUserStartedWaitsLongerAndAnAppsOwnNever) {
    panel_edge::Tracker t;
    t.update(at(0, Over::UserFullscreen), 0);
    EXPECT_EQ(t.update(at(0, Over::UserFullscreen), 399), Edge::None);
    EXPECT_EQ(t.update(at(0, Over::UserFullscreen), 400), Edge::Top);
    // The app takes the screen for itself (a video): the edge is let go.
    EXPECT_EQ(t.update(at(0, Over::AppFullscreen), 500), Edge::None);
    EXPECT_FALSE(t.wake_in(500));
    EXPECT_EQ(t.update(at(0, Over::AppFullscreen), 5000), Edge::None);
}

TEST(PanelEdge, AnotherScreenStartsAfresh) {
    panel_edge::Tracker t;
    t.update(at(1079), 0);
    ASSERT_EQ(t.update(at(1079), 200), Edge::Bottom);
    panel_edge::Pointer other = at(1079);
    other.screen = 1;
    EXPECT_EQ(t.update(other, 210), Edge::None);
    EXPECT_EQ(t.screen(), 1);
    EXPECT_EQ(t.update(other, 410), Edge::Bottom);
}

// --- Out or away (the shell's half) ------------------------------------------

TEST(Reveal, APanelThatDoesNotHideIsOut) {
    reveal::Reveal r;
    EXPECT_TRUE(r.update({}, 0));
    EXPECT_FALSE(r.wake_in(0));
}

TEST(Reveal, ToldToHideItGoesAtOnce) {
    reveal::Reveal r;
    r.update({}, 0);
    EXPECT_FALSE(r.update({.hides = true}, 10));
}

TEST(Reveal, TheEdgeBringsItAndLeavingSendsItAfterTheGrace) {
    reveal::Reveal r;
    r.update({.hides = true}, 0);
    EXPECT_TRUE(r.update({.hides = true, .edge = true}, 100));
    EXPECT_TRUE(r.update({.hides = true}, 500));
    EXPECT_DOUBLE_EQ(*r.wake_in(500), reveal::kLeaveMs);
    EXPECT_TRUE(r.update({.hides = true}, 549));
    EXPECT_FALSE(r.update({.hides = true}, 550));
    EXPECT_FALSE(r.wake_in(550));
}

TEST(Reveal, CrossingFromThePanelToItsMenuKeepsIt) {
    reveal::Reveal r;
    r.update({.hides = true}, 0);
    r.update({.hides = true, .hovered = true}, 100);
    r.update({.hides = true}, 200);                         // off the panel
    EXPECT_TRUE(r.update({.hides = true, .held = true}, 230));  // its menu opened
    EXPECT_TRUE(r.update({.hides = true, .held = true}, 5000));
    EXPECT_FALSE(r.wake_in(5000));
    // The menu closes with the pointer elsewhere.
    EXPECT_TRUE(r.update({.hides = true}, 6000));
    EXPECT_FALSE(r.update({.hides = true}, 6050));
}

TEST(Reveal, SummonedItStaysThreeSecondsAndWhileUsed) {
    reveal::Reveal r;
    r.update({.hides = true}, 0);
    EXPECT_TRUE(r.summon(1000));
    EXPECT_DOUBLE_EQ(*r.wake_in(1000), reveal::kSummonMs);
    EXPECT_TRUE(r.update({.hides = true}, 3999));
    EXPECT_TRUE(r.update({.hides = true}, 4000));  // the grace after it
    EXPECT_FALSE(r.update({.hides = true}, 4050));

    r.summon(5000);
    r.update({.hides = true, .hovered = true}, 6000);
    EXPECT_TRUE(r.update({.hides = true, .hovered = true}, 9000));
    r.update({.hides = true}, 9100);
    EXPECT_FALSE(r.update({.hides = true}, 9150));
}

TEST(Reveal, SummonedAgainItGoesThoughThePointerIsOnIt) {
    reveal::Reveal r;
    r.update({.hides = true}, 0);
    r.summon(100);
    r.update({.hides = true, .hovered = true}, 200);
    EXPECT_FALSE(r.summon(300));
    // Still under the pointer: it stays away.
    EXPECT_FALSE(r.update({.hides = true, .hovered = true}, 400));
    // Until the pointer has left and come back.
    r.update({.hides = true}, 500);
    EXPECT_TRUE(r.update({.hides = true, .hovered = true}, 600));
}

TEST(Reveal, SummonDoesNothingToAPanelThatDoesNotHideNorWithAMenuOpen) {
    reveal::Reveal r;
    r.update({}, 0);
    EXPECT_TRUE(r.summon(10));
    EXPECT_FALSE(r.wake_in(10));

    r.update({.hides = true, .held = true}, 20);
    EXPECT_TRUE(r.summon(30));  // not sent away under an open menu
    EXPECT_TRUE(r.out());
}

TEST(Reveal, NoLongerHidingForgetsTheSummon) {
    reveal::Reveal r;
    r.update({.hides = true}, 0);
    r.summon(100);
    r.update({}, 200);
    EXPECT_FALSE(r.wake_in(200));
    EXPECT_FALSE(r.update({.hides = true}, 300));
}

TEST(Reveal, PeekedItComesOutAndNeverToggles) {
    reveal::Reveal r;
    r.update({.hides = true}, 0);
    EXPECT_TRUE(r.peek(100));
    EXPECT_TRUE(r.peek(200));  // a second app asking: still out, not sent away
    EXPECT_TRUE(r.update({.hides = true}, 3199));
    EXPECT_TRUE(r.update({.hides = true}, 3200));  // the grace after it
    EXPECT_FALSE(r.update({.hides = true}, 3250));
}
