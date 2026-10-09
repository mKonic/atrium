#include "shot_core.hpp"

#include <gtest/gtest.h>

#include <filesystem>

using namespace atrium::shot;

TEST(Shot, GeometryAsSlurpPrintsIt) {
    EXPECT_EQ(parse_box("10,20 300x400"), (Box{10, 20, 300, 400}));
    EXPECT_EQ(parse_box("-1920,0 1920x1080"), (Box{-1920, 0, 1920, 1080}));
    EXPECT_FALSE(parse_box("10,20,300,400"));
    EXPECT_FALSE(parse_box("10 20 300x400"));
    EXPECT_FALSE(parse_box("10,20 300x"));
    EXPECT_FALSE(parse_box("10,20 0x400"));
    EXPECT_FALSE(parse_box("10,20 300x400 extra"));
    EXPECT_FALSE(parse_box(""));
}

TEST(Shot, WhichScreensARegionTakesIn) {
    const Box left{0, 0, 1920, 1080}, right{1920, 0, 2560, 1440};
    EXPECT_TRUE(intersects({1900, 100, 50, 50}, left));
    EXPECT_TRUE(intersects({1900, 100, 50, 50}, right));
    EXPECT_FALSE(intersects({1920, 0, 10, 10}, left));  // touching isn't taking in
    EXPECT_EQ(extents({left, right}), (Box{0, 0, 4480, 1440}));
    EXPECT_EQ(extents({{-100, -50, 100, 50}, {0, 0, 10, 10}}), (Box{-100, -50, 110, 60}));
    EXPECT_EQ(extents({}), Box{});
}

TEST(Shot, TheGreatestScale) {
    EXPECT_EQ(greatest_scale({1, 2, 1.5}), 2);
    EXPECT_EQ(greatest_scale({}), 1);
    EXPECT_EQ(greatest_scale({0.5}), 1);  // never smaller than the layout
}

TEST(Shot, FileTypesAndNames) {
    EXPECT_EQ(file_type("png"), FileType::Png);
    EXPECT_EQ(file_type("jpg"), FileType::Jpeg);
    EXPECT_FALSE(file_type("gif"));
    std::tm tm{};
    tm.tm_year = 126;
    tm.tm_mon = 9;
    tm.tm_mday = 9;
    tm.tm_hour = 14;
    tm.tm_min = 3;
    tm.tm_sec = 22;
    tm.tm_isdst = -1;
    EXPECT_EQ(default_name(FileType::Png, std::mktime(&tm)), "20261009_14h03m22s_atrium.png");
    EXPECT_EQ(default_name(FileType::Jpeg, std::mktime(&tm)), "20261009_14h03m22s_atrium.jpeg");
}

TEST(Shot, WherePicturesGo) {
    const auto tmp = std::filesystem::temp_directory_path() / "atrium-shot-test";
    std::filesystem::create_directories(tmp / "Pictures");
    const std::string home = tmp.string();
    // $GRIM_DEFAULT_DIR first, when it exists.
    EXPECT_EQ(pictures_dir(home.c_str(), "", home.c_str()), home);
    EXPECT_EQ(pictures_dir("/nonexistent", "XDG_PICTURES_DIR=\"$HOME/Pictures\"\n", home.c_str()), home + "/Pictures");
    // Comments and other keys don't count; a missing folder falls back here.
    EXPECT_EQ(pictures_dir(nullptr, "# XDG_PICTURES_DIR=\"$HOME/Pictures\"\n", home.c_str()), ".");
    EXPECT_EQ(pictures_dir(nullptr, "XDG_PICTURES_DIR=\"$HOME/Nope\"\n", home.c_str()), ".");
    std::filesystem::remove_all(tmp);
}
