#include "util/xcursor.hpp"

#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <fstream>

using namespace atrium;
namespace fs = std::filesystem;

namespace {

struct Frame {
    uint32_t size, w, h, delay, pixel;
};

// An Xcursor file with `frames` (each an image chunk of its nominal size).
std::string xcursor_file(const std::vector<Frame>& frames) {
    std::string out;
    auto u32 = [&](uint32_t v) {
        for (int i = 0; i < 4; ++i)
            out.push_back(char(v >> (8 * i)));
    };
    u32(0x72756358);
    u32(16);
    u32(0x10000);
    u32(uint32_t(frames.size()));
    uint32_t pos = 16 + 12 * uint32_t(frames.size());
    for (const Frame& f : frames) {
        u32(0xfffd0002);
        u32(f.size);
        u32(pos);
        pos += 36 + 4 * f.w * f.h;
    }
    for (const Frame& f : frames) {
        u32(36);
        u32(0xfffd0002);
        u32(f.size);
        u32(1);
        u32(f.w);
        u32(f.h);
        u32(1);  // hotspot
        u32(2);
        u32(f.delay);
        for (uint32_t i = 0; i < f.w * f.h; ++i)
            u32(f.pixel);
    }
    return out;
}

std::vector<xcursor::Image> read(const std::string& bytes, uint32_t size) {
    FILE* f = fmemopen(const_cast<char*>(bytes.data()), bytes.size(), "rb");
    auto out = xcursor::read_file(f, size);
    fclose(f);
    return out;
}

TEST(Xcursor, ReadsTheNearestSizesFrames) {
    const std::string file = xcursor_file({{24, 4, 4, 0, 0xff000001},
                                           {48, 8, 8, 50, 0xff000002},
                                           {48, 8, 8, 70, 0xff000003}});
    auto small = read(file, 30);
    ASSERT_EQ(small.size(), 1u);
    EXPECT_EQ(small[0].width, 4u);
    EXPECT_EQ(small[0].hotspot_y, 2u);
    EXPECT_EQ(small[0].pixels.size(), 16u);
    EXPECT_EQ(small[0].pixels[5], 0xff000001u);
    auto big = read(file, 40);  // the animated one
    ASSERT_EQ(big.size(), 2u);
    EXPECT_EQ(big[0].delay, 50u);
    EXPECT_EQ(big[1].pixels[0], 0xff000003u);
}

TEST(Xcursor, RejectsBrokenFiles) {
    EXPECT_TRUE(read("not a cursor file at all", 24).empty());
    std::string file = xcursor_file({{24, 4, 4, 0, 1}});
    file.resize(file.size() - 8);  // pixels cut short
    EXPECT_TRUE(read(file, 24).empty());
}

TEST(Xcursor, Inherits) {
    EXPECT_EQ(xcursor::inherits("[Icon Theme]\nName=x\nInherits = Adwaita, hicolor;core\n"),
              (std::vector<std::string>{"Adwaita", "hicolor", "core"}));
    EXPECT_TRUE(xcursor::inherits("[Icon Theme]\nInheritsFrom=x\n").empty());
}

class XcursorThemeTest : public ::testing::Test {
protected:
    void SetUp() override {
        root = fs::temp_directory_path() / ("atrium-xcursor-" + std::to_string(getpid()));
        fs::create_directories(root / "child/cursors");
        fs::create_directories(root / "parent/cursors");
        std::ofstream(root / "child/index.theme") << "[Icon Theme]\nInherits=parent\n";
        std::ofstream(root / "parent/index.theme") << "[Icon Theme]\nInherits=child\n";  // a loop
        std::ofstream(root / "child/cursors/left_ptr") << xcursor_file({{24, 2, 2, 0, 0xff0000ff}});
        std::ofstream(root / "parent/cursors/left_ptr") << xcursor_file({{24, 2, 2, 0, 0xffff0000}});
        std::ofstream(root / "parent/cursors/text") << xcursor_file({{24, 3, 3, 0, 0xff00ff00}});
    }
    void TearDown() override { fs::remove_all(root); }
    fs::path root;
};

TEST_F(XcursorThemeTest, OwnCursorsWinOverInheritedOnes) {
    xcursor::Theme t("child", 24, {root.string()});
    EXPECT_FALSE(t.fallback());
    EXPECT_EQ(t.count(), 2u);
    const xcursor::Cursor* ptr = t.get("default");  // by its legacy name
    ASSERT_NE(ptr, nullptr);
    EXPECT_EQ(ptr->images[0].pixels[0], 0xff0000ffu);
    ASSERT_NE(t.get("text"), nullptr);
    EXPECT_EQ(t.get("text")->images[0].width, 3u);
    EXPECT_EQ(t.get("wait"), nullptr);
}

TEST_F(XcursorThemeTest, NoThemeDrawsAnArrow) {
    xcursor::Theme t("nonexistent", 32, {root.string()});
    EXPECT_TRUE(t.fallback());
    const xcursor::Cursor* c = t.get("anything");
    ASSERT_NE(c, nullptr);
    const xcursor::Image& img = c->images[0];
    EXPECT_EQ(img.width, 32u);
    // Its tip is drawn, its far corner isn't.
    EXPECT_NE(img.pixels[size_t(4 * 32 + 3)] >> 24, 0u);
    EXPECT_EQ(img.pixels.back(), 0u);
}

TEST_F(XcursorThemeTest, ManagerLoadsPerScale) {
    setenv("XCURSOR_PATH", root.c_str(), 1);
    xcursor::Manager m("child", 24);
    const xcursor::Cursor* a = m.get("text", 1);
    EXPECT_EQ(m.get("text", 1), a);
    EXPECT_NE(m.get("text", 2), nullptr);
    EXPECT_NE(m.get("text", 2), a);
    unsetenv("XCURSOR_PATH");
}

} // namespace
