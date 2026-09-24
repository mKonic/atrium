#include "accent.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdlib>

using namespace atrium::accent;

namespace {

int channel(uint32_t rgb, int shift) {
    return int((rgb >> shift) & 0xff);
}

int max_channel_gap(uint32_t a, uint32_t b) {
    int gap = 0;
    for (int s : {0, 8, 16})
        gap = std::max(gap, std::abs(channel(a, s) - channel(b, s)));
    return gap;
}

} // namespace

TEST(Accent, NamesAndSeeds) {
    EXPECT_EQ(names().front(), "multicolor");
    EXPECT_EQ(names().size(), 9u);
    EXPECT_FALSE(seed("multicolor"));
    EXPECT_FALSE(seed("chartreuse"));
    EXPECT_EQ(seed("blue"), 0x0a84ffu);
    EXPECT_EQ(gnome_name("graphite"), "slate");
    EXPECT_EQ(label("graphite"), "Graphite");
    EXPECT_EQ(label("multicolor"), "Multicolour");
    EXPECT_EQ(gnome_name("multicolor"), "blue");
}

TEST(Accent, ToneHitsTheLightnessAsked) {
    for (std::string_view n : names())
        if (auto s = seed(n))
            for (double l : {10.0, 20.0, 40.0, 50.0, 80.0, 90.0})
                EXPECT_NEAR(lightness(tone(*s, l)), l, 1.0) << n << " at " << l;
    EXPECT_LE(max_channel_gap(tone(0xff453a, 100), 0xffffff), 1);
    EXPECT_LE(max_channel_gap(tone(0xff453a, 0), 0x000000), 1);
}

TEST(Accent, ASeedAtItsOwnToneIsItself) {
    for (std::string_view n : names())
        if (auto s = seed(n))
            EXPECT_LE(max_channel_gap(tone(*s, lightness(*s)), *s), 2) << n;
}

TEST(Accent, KeepsTheHue) {
    const uint32_t red = tone(0xff453a, 40), blue = tone(0x0a84ff, 40), green = tone(0x32d74b, 40);
    EXPECT_GT(channel(red, 16), channel(red, 0));
    EXPECT_GT(channel(blue, 0), channel(blue, 16));
    EXPECT_GT(channel(green, 8), channel(green, 16));
    // Graphite stays grey, and secondary surfaces are only tinted.
    const uint32_t grey = tone(0x8e8e93, 80);
    EXPECT_LE(max_channel_gap(grey, (grey & 0xff) * 0x010101u), 8);
    const Tones t = tones(0xff453a, false);
    EXPECT_LT(max_channel_gap(t.secondary_container, (t.secondary_container & 0xff) * 0x010101u),
              max_channel_gap(t.primary_container, (t.primary_container & 0xff) * 0x010101u));
}

TEST(Accent, TextStandsOutFromItsBackground) {
    for (bool light : {false, true})
        for (std::string_view n : names())
            if (auto s = seed(n)) {
                const Tones t = tones(*s, light);
                EXPECT_GE(std::abs(lightness(t.primary) - lightness(t.on_primary)), 50) << n;
                EXPECT_GE(std::abs(lightness(t.primary_container) - lightness(t.on_primary_container)), 39) << n;
                EXPECT_GE(std::abs(lightness(t.secondary_container) - lightness(t.on_secondary_container)), 50) << n;
            }
}
