#include "battery_core.hpp"

#include <gtest/gtest.h>

using namespace atrium::battery;

TEST(Battery, Glyphs) {
    EXPECT_EQ(glyph(100, State::Discharging), "battery_full");
    EXPECT_EQ(glyph(50, State::Discharging), "battery_3_bar");
    EXPECT_EQ(glyph(11, State::Discharging), "battery_0_bar");
    EXPECT_EQ(glyph(8, State::Discharging), "battery_alert");
    EXPECT_EQ(glyph(55, State::Charging), "battery_charging_50");
    EXPECT_EQ(glyph(5, State::Charging), "battery_charging_20");
    EXPECT_EQ(glyph(80, State::Full), "battery_charging_full");
    EXPECT_EQ(glyph(80, State::PendingCharge), "battery_charging_80");
}

TEST(Battery, Remaining) {
    EXPECT_EQ(remaining(State::Discharging, 2 * 3600 + 5 * 60, 0), "2:05 left");
    EXPECT_EQ(remaining(State::Charging, 0, 100 * 60), "1:40 until full");
    EXPECT_EQ(remaining(State::Charging, 0, 0), "Charging");
    EXPECT_EQ(remaining(State::Full, 0, 0), "Fully charged");
    EXPECT_EQ(remaining(State::PendingCharge, 0, 0), "Not charging");
    EXPECT_EQ(remaining(State::Discharging, 0, 0), "");
}

TEST(Battery, WarnsOncePerLevel) {
    EXPECT_EQ(warning(50, State::Discharging, 0), 0);
    EXPECT_EQ(warning(10, State::Discharging, 0), 1);
    EXPECT_EQ(warning(9, State::Discharging, 1), 0);
    EXPECT_EQ(warning(5, State::Discharging, 1), 2);
    EXPECT_EQ(warning(3, State::Discharging, 2), 0);
    EXPECT_EQ(warning(3, State::Charging, 0), 0);
}
