#include "access_keys_core.hpp"

#include <gtest/gtest.h>

using namespace atrium::typing;

namespace {
constexpr uint32_t kA = 30, kB = 48;
constexpr uint32_t kShift = 1u << 0, kCtrl = 1u << 2;
} // namespace

TEST(BounceKeys, DropsAQuickSecondPressAndItsRelease) {
    BounceKeys b;
    EXPECT_FALSE(b.filter(kA, true, 1000, 500));
    EXPECT_FALSE(b.filter(kA, false, 1050, 500));
    // Again 200 ms after the first press: a bounce.
    EXPECT_TRUE(b.filter(kA, true, 1200, 500));
    EXPECT_TRUE(b.filter(kA, false, 1250, 500));
    // Another key isn't.
    EXPECT_FALSE(b.filter(kB, true, 1260, 500));
    // Long enough after the last (bounced) press: typed.
    EXPECT_FALSE(b.filter(kA, true, 1800, 500));
    EXPECT_FALSE(b.filter(kA, false, 1850, 500));
}

TEST(SlowKeys, AKeyCountsOnceHeldForTheDelay) {
    SlowKeys s;
    s.press(kA);
    EXPECT_TRUE(s.waiting(kA));
    EXPECT_TRUE(s.expire(kA));   // still down: its press goes out
    EXPECT_FALSE(s.expire(kA));  // only once
    EXPECT_TRUE(s.release(kA));  // and its release
    // Let go too soon: neither goes out.
    s.press(kB);
    EXPECT_FALSE(s.release(kB));
    EXPECT_FALSE(s.expire(kB));
    // A key down from before slow keys was turned on: its release is its own.
    EXPECT_TRUE(s.release(kA));
}

TEST(StickyKeys, LatchesForTheNextKeyAndLocksOnASecondPress) {
    StickyKeys s;
    s.modifier_pressed(kShift);
    s.modifier_released(kShift);
    EXPECT_EQ(s.latched(), kShift);
    EXPECT_FALSE(s.key_pressed());
    EXPECT_EQ(s.latched(), 0u);
    // Twice: locked, through any number of keys, until pressed again.
    s.modifier_pressed(kCtrl);
    s.modifier_released(kCtrl);
    s.modifier_pressed(kCtrl);
    s.modifier_released(kCtrl);
    EXPECT_EQ(s.locked(), kCtrl);
    EXPECT_EQ(s.latched(), 0u);
    s.key_pressed();
    s.button_released();
    EXPECT_EQ(s.locked(), kCtrl);
    s.modifier_pressed(kCtrl);
    EXPECT_EQ(s.locked(), 0u);
    EXPECT_EQ(s.managed(), kShift | kCtrl);
}

TEST(StickyKeys, WithoutLockingASecondPressLetsGo) {
    StickyKeys s;
    s.set_options({.lock = false});
    s.modifier_pressed(kShift);
    s.modifier_pressed(kShift);
    EXPECT_EQ(s.latched(), 0u);
    EXPECT_EQ(s.locked(), 0u);
}

TEST(StickyKeys, AClickEndsALatch) {
    StickyKeys s;
    s.modifier_pressed(kCtrl);
    s.modifier_released(kCtrl);
    s.button_released();
    EXPECT_EQ(s.latched(), 0u);
}

TEST(StickyKeys, TwoKeysTogetherTurnItOffWhenAsked) {
    StickyKeys s;
    s.set_options({.lock = true, .auto_off = true});
    s.modifier_pressed(kShift);  // held...
    EXPECT_TRUE(s.key_pressed());  // ...with another key: off
    EXPECT_EQ(s.latched(), 0u);
    // Without auto_off, holding Shift and typing is just Shift.
    StickyKeys t;
    t.modifier_pressed(kShift);
    EXPECT_FALSE(t.key_pressed());
}
