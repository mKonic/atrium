#include "share_core.hpp"

#include <gtest/gtest.h>

using namespace atrium::share;

TEST(Share, Parses) {
    const auto s = parse("Monitor: DP-1 Dell Inc. DELL U2720Q 1234\n"
                         "Monitor: WL-1\n"
                         "Window: README (notes).md - Kate (3f2a91c0)\n"
                         "Window:  (00ff)\n");
    ASSERT_EQ(s.size(), 4u);
    EXPECT_EQ(s[0].kind, Source::Kind::Screen);
    EXPECT_EQ(s[0].name, "DP-1");
    EXPECT_EQ(s[0].text, "Dell Inc. DELL U2720Q 1234");
    EXPECT_EQ(s[0].line, "Monitor: DP-1 Dell Inc. DELL U2720Q 1234");
    EXPECT_EQ(s[1].name, "WL-1");
    EXPECT_EQ(s[1].text, "");
    EXPECT_EQ(s[2].kind, Source::Kind::Window);
    EXPECT_EQ(s[2].name, "3f2a91c0");
    EXPECT_EQ(s[2].text, "README (notes).md - Kate");
    EXPECT_EQ(s[3].name, "00ff");
    EXPECT_EQ(s[3].text, "");
}

TEST(Share, SkipsOtherLines) {
    EXPECT_TRUE(parse("").empty());
    EXPECT_TRUE(parse("hello\nMonitor: \nWindow: no id\nWindow: empty ()\n").empty());
    EXPECT_EQ(parse("Monitor: HDMI-A-1 TV\r\n")[0].text, "TV");
}
