#include "../src/idle_core.hpp"

#include <gtest/gtest.h>

using namespace atrium;

namespace {

constexpr uint32_t bit(IdleStage s) { return 1u << int(s); }

TEST(Idle, ReadsTheSettingsChoices) {
    EXPECT_EQ(idle_after_seconds("never"), 0);
    EXPECT_EQ(idle_after_seconds("1-minute"), 60);
    EXPECT_EQ(idle_after_seconds("15-minutes"), 900);
    EXPECT_EQ(idle_after_seconds("1-hour"), 3600);
    EXPECT_EQ(idle_after_seconds("3-hours"), 10800);
    EXPECT_EQ(idle_after_seconds("5-days"), 0);
    EXPECT_EQ(idle_after_seconds("-minutes"), 0);
    EXPECT_EQ(idle_after_seconds("0-minutes"), 0);
    EXPECT_EQ(idle_after_seconds(""), 0);
}

TEST(Idle, NothingSetMeansNothingDue) {
    const IdleDue due = idle_due({}, 1'000'000'000, 0);
    EXPECT_EQ(due.now, 0u);
    EXPECT_EQ(due.next_ms, -1);
}

TEST(Idle, StagesComeDueInTurn) {
    IdleTimes t;
    t.seconds[int(IdleStage::Dim)] = 60;
    t.seconds[int(IdleStage::ScreenOff)] = 120;
    t.seconds[int(IdleStage::Suspend)] = 1800;

    IdleDue due = idle_due(t, 0, 0);
    EXPECT_EQ(due.now, 0u);
    EXPECT_EQ(due.next_ms, 60'000);

    due = idle_due(t, 60'000, 0);
    EXPECT_EQ(due.now, bit(IdleStage::Dim));
    EXPECT_EQ(due.next_ms, 60'000);

    // Done once, a stage isn't due again until input clears it.
    due = idle_due(t, 130'000, bit(IdleStage::Dim));
    EXPECT_EQ(due.now, bit(IdleStage::ScreenOff));
    EXPECT_EQ(due.next_ms, 1'670'000);

    due = idle_due(t, 2'000'000, bit(IdleStage::Dim) | bit(IdleStage::ScreenOff) | bit(IdleStage::Suspend));
    EXPECT_EQ(due.now, 0u);
    EXPECT_EQ(due.next_ms, -1);
}

TEST(Idle, LateWakeupsCatchUpEverything) {
    IdleTimes t;
    t.seconds[int(IdleStage::Dim)] = 60;
    t.seconds[int(IdleStage::Lock)] = 300;
    const IdleDue due = idle_due(t, 400'000, 0);
    EXPECT_EQ(due.now, bit(IdleStage::Dim) | bit(IdleStage::Lock));
    EXPECT_EQ(due.next_ms, -1);
}

} // namespace
