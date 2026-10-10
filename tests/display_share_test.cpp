#include "display_share.hpp"

#include <gtest/gtest.h>

using namespace atrium;
using nlohmann::json;

TEST(DisplayShare, TheLoginScreenGetsTheSessionsSetup) {
    DisplayRecord msi{.id = "Microstep G274F CC2H564A00361", .width = 1920, .height = 1080, .refresh = 179999,
                      .x = 0, .y = 0, .hdr = true, .sdr_brightness = 100, .icc = "/home/u/msi.icc"};
    DisplayRecord dell{.id = "Dell Inc. DELL P2219H 2D7JYS2", .enabled = false, .width = 1920, .height = 1080,
                       .refresh = 60000, .transform = 1};
    const auto back = displays_from_json(json::parse(displays_line({msi, dell}).substr(9)));
    ASSERT_EQ(back.size(), 2u);
    EXPECT_EQ(back[0].refresh, 179999);
    EXPECT_TRUE(back[0].hdr);
    EXPECT_EQ(back[0].x, 0);
    // A colour profile is a path in the user's home: not handed over.
    EXPECT_EQ(back[0].icc, "");
    EXPECT_FALSE(back[1].enabled);
    EXPECT_EQ(back[1].transform, 1);
    EXPECT_FALSE(back[1].x.has_value());
}

// The file sits in the login screen's home, written from what a session
// sent: anything malformed is dropped, out-of-range values clamped.
TEST(DisplayShare, KeepsOnlyWhatsWellFormed) {
    const auto got = displays_from_json(json::parse(R"([
        {"id": ""},
        {"no": "id"},
        "text",
        {"id": "A", "hdr": "yes", "enabled": 1, "scale": 100, "transform": 9, "refresh": -5, "adaptive_sync": "always"},
        {"id": "B", "x": 10}
    ])"));
    ASSERT_EQ(got.size(), 2u);
    EXPECT_EQ(got[0].id, "A");
    EXPECT_FALSE(got[0].hdr);
    EXPECT_TRUE(got[0].enabled);
    EXPECT_EQ(got[0].scale, 8.0);
    EXPECT_EQ(got[0].transform, 7);
    EXPECT_EQ(got[0].refresh, 0);
    EXPECT_EQ(got[0].adaptive_sync, "games");
    EXPECT_FALSE(got[1].x.has_value());  // a position needs both
    EXPECT_TRUE(displays_from_json(json::object()).empty());
}
