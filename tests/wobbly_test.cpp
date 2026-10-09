#include "../src/wobbly_core.hpp"

#include <gtest/gtest.h>

using namespace atrium;

namespace {

TEST(Wobbly, AtRestTheSurfaceIsTheWindow) {
    const FBox r{100, 100, 300, 200};
    Wobbly w(r, wobbly_preset(0), {110, 110}, false);
    EXPECT_TRUE(w.advance(r, 50));
    EXPECT_FALSE(w.wobbling());
    const FPoint c = w.at(0.5, 0.5);
    EXPECT_NEAR(c.x, 250, 1e-9);
    EXPECT_NEAR(c.y, 200, 1e-9);
    const FPoint br = w.at(1, 1);
    EXPECT_NEAR(br.x, 400, 1e-9);
    EXPECT_NEAR(br.y, 300, 1e-9);
}

TEST(Wobbly, TheFarSideLagsBehindTheHeldPoint) {
    // Held by the top left and dragged right: the top left keeps up, the
    // bottom right trails.
    FBox r{100, 100, 300, 200};
    Wobbly w(r, wobbly_preset(0), {100, 100}, false);
    for (int i = 0; i < 10; i++) {
        r.x += 20;
        w.advance(r, 16);
    }
    EXPECT_TRUE(w.wobbling());
    const double lag_held = r.x - w.at(0, 0).x;
    const double lag_far = r.x + r.width - w.at(1, 1).x;
    EXPECT_GT(lag_far, lag_held);
    EXPECT_GT(lag_far, 5);
}

TEST(Wobbly, ItSettlesAfterTheRelease) {
    FBox r{100, 100, 300, 200};
    Wobbly w(r, wobbly_preset(0), {250, 200}, false);
    for (int i = 0; i < 10; i++) {
        r.y += 30;
        w.advance(r, 16);
    }
    w.release(r);
    int ms = 0;
    while (w.advance(r, 16) && ms < 10000)
        ms += 16;
    EXPECT_LT(ms, 10000);
    EXPECT_NEAR(w.at(1, 1).x, 400, 1);
    EXPECT_NEAR(w.at(1, 1).y, r.y + 200, 1);
}

TEST(Wobbly, AResizeWobblesOnlyTheSideThatMoved) {
    // Dragging the right edge: the left side stays where it was.
    FBox r{100, 100, 300, 200};
    Wobbly w(r, wobbly_preset(0), {400, 200}, true);
    for (int i = 0; i < 10; i++) {
        r.width += 20;
        w.moved(r);
        w.advance(r, 16);
    }
    EXPECT_NEAR(w.at(0, 0.5).x, 100, 1e-9);
    EXPECT_NEAR(w.at(0.5, 0).y, 100, 1e-9);
}

TEST(Wobbly, StifferLevelsSettleSooner) {
    auto settle = [](int level) {
        FBox r{0, 0, 300, 200};
        Wobbly w(r, wobbly_preset(level), {0, 0}, false);
        for (int i = 0; i < 10; i++) {
            r.x += 30;
            w.advance(r, 16);
        }
        w.release(r);
        int ms = 0;
        while (w.advance(r, 16) && ms < 20000)
            ms += 16;
        return ms;
    };
    EXPECT_LT(settle(0), settle(4));
}

} // namespace
