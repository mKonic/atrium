#include "night_light_core.hpp"

#include <gtest/gtest.h>

using namespace atrium::night;

TEST(NightLight, Whitepoint) {
    const Rgb day = whitepoint(6500);
    EXPECT_DOUBLE_EQ(day.r, 1);
    EXPECT_DOUBLE_EQ(day.b, 1);
    const Rgb warm = whitepoint(3000);
    EXPECT_DOUBLE_EQ(warm.r, 1);
    EXPECT_LT(warm.g, 0.85);
    EXPECT_LT(warm.b, warm.g);
    EXPECT_GT(whitepoint(4500).b, warm.b);  // less warm, more blue
    EXPECT_EQ(kelvin_for(0), 5500);
    EXPECT_EQ(kelvin_for(100), 2500);
    EXPECT_EQ(kelvin_for(250), 2500);
}

TEST(NightLight, Coordinates) {
    auto paris = parse_iso6709("+4852+00220");
    ASSERT_TRUE(paris);
    EXPECT_NEAR(paris->lat, 48.8667, 1e-3);
    EXPECT_NEAR(paris->lon, 2.3333, 1e-3);
    auto santiago = parse_iso6709("-332751-0703940");
    ASSERT_TRUE(santiago);
    EXPECT_NEAR(santiago->lat, -33.4642, 1e-3);
    EXPECT_NEAR(santiago->lon, -70.6611, 1e-3);
    EXPECT_FALSE(parse_iso6709("4852+00220"));
    EXPECT_FALSE(parse_iso6709("+48x2+00220"));

    const char* tab = "# comment\n"
                      "FR,MC\t+4852+00220\tEurope/Paris\n"
                      "KE,DJ,ER\t-0117+03649\tAfrica/Nairobi\tsome words\n";
    auto nairobi = zone_coordinates(tab, "Africa/Nairobi");
    ASSERT_TRUE(nairobi);
    EXPECT_NEAR(nairobi->lat, -1.2833, 1e-3);
    EXPECT_FALSE(zone_coordinates(tab, "Europe/Berlin"));
}

TEST(NightLight, Sun) {
    // Paris at midsummer: up about 03:47 UTC, down about 19:58 UTC.
    const SunTimes paris = sun_times({48.8667, 2.3333}, 2026, 6, 21);
    EXPECT_NEAR(paris.rise, 3 * 60 + 47, 6);
    EXPECT_NEAR(paris.set, 19 * 60 + 58, 6);
    // Nairobi, near the equator: about 03:30 and 15:40 UTC all year.
    const SunTimes nairobi = sun_times({-1.2833, 36.8167}, 2026, 9, 24);
    EXPECT_NEAR(nairobi.rise, 3 * 60 + 25, 10);
    EXPECT_NEAR(nairobi.set, 15 * 60 + 35, 10);
    // Tromsø: midnight sun in June, polar night in December.
    EXPECT_TRUE(sun_times({69.65, 18.96}, 2026, 6, 21).always_up);
    EXPECT_TRUE(sun_times({69.65, 18.96}, 2026, 12, 21).always_down);
}

TEST(NightLight, Windows) {
    EXPECT_EQ(parse_hhmm("22:00"), 22 * 60);
    EXPECT_EQ(parse_hhmm("7:05"), 7 * 60 + 5);
    EXPECT_FALSE(parse_hhmm("24:00"));
    EXPECT_FALSE(parse_hhmm("7:5"));
    EXPECT_FALSE(parse_hhmm("ab:cd"));
    EXPECT_TRUE(in_window(23 * 60, 22 * 60, 7 * 60));
    EXPECT_TRUE(in_window(3 * 60, 22 * 60, 7 * 60));
    EXPECT_FALSE(in_window(12 * 60, 22 * 60, 7 * 60));
    EXPECT_TRUE(in_window(13 * 60, 12 * 60, 14 * 60));
    EXPECT_FALSE(in_window(14 * 60, 12 * 60, 14 * 60));
    EXPECT_FALSE(in_window(5, 60, 60));
}
