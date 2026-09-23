#include "placements.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

using namespace atrium;
namespace fs = std::filesystem;

namespace {

fs::path scratch(const char* name) {
    const fs::path dir = fs::temp_directory_path() / ("atrium-test-" + std::to_string(::getpid()));
    fs::create_directories(dir);
    const fs::path f = dir / name;
    fs::remove(f);
    return f;
}

} // namespace

TEST(Placements, SurviveARestart) {
    const fs::path f = scratch("placements.json");
    {
        Placements p(f);
        p.remember("foot", {"DP-1", 100, 80, 700, 550, false, 0});
        p.remember("org.kde.dolphin", {"DP-1", 0, 0, 900, 600, true, 0});
        p.remember("foot", {"DP-1", 120, 90, 700, 550, false, 0});  // the last close wins
    }
    Placements again(f);
    ASSERT_NE(again.find("foot"), nullptr);
    EXPECT_EQ(*again.find("foot"), (Placement{"DP-1", 120, 90, 700, 550, false, 0}));
    ASSERT_NE(again.find("org.kde.dolphin"), nullptr);
    EXPECT_TRUE(again.find("org.kde.dolphin")->maximized);
    EXPECT_EQ(again.find("nope"), nullptr);
}

TEST(Placements, IgnoresNonsense) {
    const fs::path f = scratch("broken.json");
    std::ofstream(f) << "{ not json";
    Placements p(f);
    EXPECT_EQ(p.find("foot"), nullptr);
    p.remember("", {"DP-1", 0, 0, 10, 10, false, 0});    // no app id
    p.remember("foot", {"DP-1", 0, 0, 0, 10, false, 0}); // no size
    EXPECT_EQ(p.find(""), nullptr);
    EXPECT_EQ(p.find("foot"), nullptr);
}
