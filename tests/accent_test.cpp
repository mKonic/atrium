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
    for (std::string_view n : names()) {
        if (auto s = seed(n)) {
            for (double l : {10.0, 20.0, 40.0, 50.0, 80.0, 90.0})
                EXPECT_NEAR(lightness(tone(*s, l)), l, 1.0) << n << " at " << l;
        }
    }
    EXPECT_LE(max_channel_gap(tone(0xff453a, 100), 0xffffff), 1);
    EXPECT_LE(max_channel_gap(tone(0xff453a, 0), 0x000000), 1);
}

TEST(Accent, ASeedAtItsOwnToneIsItself) {
    for (std::string_view n : names()) {
        if (auto s = seed(n)) {
            EXPECT_LE(max_channel_gap(tone(*s, lightness(*s)), *s), 2) << n;
        }
    }
}

TEST(Accent, KeepsTheHue) {
    const uint32_t red = tone(0xff453a, 40), blue = tone(0x0a84ff, 40), green = tone(0x32d74b, 40);
    EXPECT_GT(channel(red, 16), channel(red, 0));
    EXPECT_GT(channel(blue, 0), channel(blue, 16));
    EXPECT_GT(channel(green, 8), channel(green, 16));
    // Graphite stays grey.
    const uint32_t grey = tone(0x8e8e93, 80);
    EXPECT_LE(max_channel_gap(grey, (grey & 0xff) * 0x010101u), 8);
}

TEST(Accent, EachAppearanceHasItsOwnShade) {
    EXPECT_EQ(rgb("blue", false), 0x0a84ffu);
    EXPECT_EQ(rgb("blue", true), 0x007affu);
    EXPECT_EQ(rgb("multicolor", true), rgb("blue", true));
    EXPECT_EQ(rgb("chartreuse", false), rgb("blue", false));
    for (std::string_view n : names()) {
        if (auto s = seed(n)) {
            EXPECT_EQ(rgb(n, false), *s) << n;
        }
    }
}

#include "palette.hpp"

TEST(Palette, MacosRoles) {
    using namespace atrium::palette;
    const Palette dark = make(false, "multicolor"), light = make(true, "no-such-accent");
    EXPECT_EQ(dark.label, 0xffffffd8u);
    EXPECT_EQ(light.label, 0x000000d8u);
    EXPECT_EQ(dark.accent, 0x0a84ffffu);
    EXPECT_EQ(light.accent, 0x007affffu);
    EXPECT_EQ(dark.red, 0xff453affu);
    EXPECT_EQ(light.red, 0xff3b30ffu);
    // Labels and fills step down in strength.
    for (const Palette& p : {dark, light}) {
        EXPECT_GT(p.label & 0xff, p.secondary_label & 0xff);
        EXPECT_GT(p.secondary_label & 0xff, p.tertiary_label & 0xff);
        EXPECT_GT(p.tertiary_label & 0xff, p.quaternary_label & 0xff);
        EXPECT_GT(p.fill & 0xff, p.secondary_fill & 0xff);
        EXPECT_GT(p.secondary_fill & 0xff, p.tertiary_fill & 0xff);
        EXPECT_GT(p.tertiary_fill & 0xff, p.quaternary_fill & 0xff);
    }
}

TEST(Palette, AnAccentOnlyChangesTheAccentRoles) {
    using namespace atrium::palette;
    const Palette base = make(false, "multicolor"), red = make(false, "red");
    EXPECT_EQ(red.accent, 0xff453affu);
    EXPECT_EQ(red.accent_fill >> 8, red.accent >> 8);
    EXPECT_LT(red.accent_fill & 0xff, 0xffu);
    EXPECT_EQ(red.label, base.label);
    EXPECT_EQ(red.window_background, base.window_background);
    EXPECT_EQ(red.separator, base.separator);
}

TEST(Palette, TextOnTheAccentReads) {
    using namespace atrium::palette;
    EXPECT_EQ(make(false, "blue").on_accent, 0xffffffffu);
    EXPECT_EQ(make(true, "green").on_accent, 0xffffffffu);
    EXPECT_EQ(make(true, "yellow").on_accent >> 8, 0u);
    EXPECT_EQ(make(false, "yellow").on_accent >> 8, 0u);
}

TEST(Palette, HexAndOver) {
    using namespace atrium::palette;
    EXPECT_EQ(hex(0x0a84ffff), "#0a84ff");
    EXPECT_EQ(hex(0xffffff8c), "#8cffffff");
    EXPECT_EQ(over(0xffffff80, 0x000000ff), 0x808080ffu);
    EXPECT_EQ(over(0x123456ff, 0xabcdefff), 0x123456ffu);
}

TEST(Palette, KdeColorsCarryThePalette) {
    const std::string c = atrium::palette::kde_colors(false, "green");
    const auto p = atrium::palette::make(false, "green");
    for (const char* group : {"[Colors:Window]", "[Colors:View]", "[Colors:Button]", "[Colors:Selection]",
                              "[Colors:Tooltip]", "[Colors:Header]", "[Colors:Complementary]", "[WM]", "[General]"})
        EXPECT_NE(c.find(group), std::string::npos) << group;
    const size_t sel = c.find("[Colors:Selection]");
    EXPECT_EQ(c.find("BackgroundNormal=" + atrium::palette::hex(p.accent), sel), c.find("BackgroundNormal", sel));
    const size_t view = c.find("[Colors:View]");
    EXPECT_EQ(c.find("BackgroundNormal=" + atrium::palette::hex(p.control_background), view), c.find("BackgroundNormal", view));
    // KDE only takes opaque colours.
    for (size_t at = c.find("=#"); at != std::string::npos; at = c.find("=#", at + 1))
        EXPECT_EQ(c.find('\n', at) - at, 8u) << c.substr(at, 12);
}
