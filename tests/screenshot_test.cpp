#include "../src/screenshot_core.hpp"

#include <gtest/gtest.h>

#include <algorithm>

using namespace atrium;

namespace {

// A w x h buffer whose pixels say where they are: y * 16 + x.
std::vector<uint32_t> numbered(int w, int h) {
    std::vector<uint32_t> v(size_t(w * h));
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            v[size_t(y * w + x)] = uint32_t(y * 16 + x);
    return v;
}

std::vector<uint32_t> upright(const std::vector<uint32_t>& buf, int bw, int bh, wl_output_transform t, int* w,
                              int* h) {
    return screenshot_upright(reinterpret_cast<const uint8_t*>(buf.data()), bw, bh, size_t(bw) * 4, t, w, h);
}

TEST(Screenshot, TheAreaIsEveryScreenTogether) {
    const Box a = screenshot_area({{0, 0, 1440, 900}, {1440, -200, 720, 1280}}, std::nullopt);
    EXPECT_EQ(a, (Box{0, -200, 2160, 1280}));
}

TEST(Screenshot, ARegionIsCutToTheScreens) {
    const std::vector<Box> screens{{0, 0, 1440, 900}};
    EXPECT_EQ(screenshot_area(screens, Box{100, 50, 200, 100}), (Box{100, 50, 200, 100}));
    EXPECT_EQ(screenshot_area(screens, Box{1400, 850, 200, 100}), (Box{1400, 850, 40, 50}));
    const Box off = screenshot_area(screens, Box{2000, 0, 10, 10});
    EXPECT_TRUE(off.width <= 0 || off.height <= 0);
}

TEST(Screenshot, ScreensThatAreOffDontCount) {
    EXPECT_EQ(screenshot_area({{0, 0, 0, 0}, {100, 100, 50, 50}}, std::nullopt), (Box{100, 100, 50, 50}));
    const Box none = screenshot_area({}, std::nullopt);
    EXPECT_LE(none.width, 0);
}

TEST(Screenshot, AnUnturnedFrameIsCopiedAsItIs) {
    int w = 0, h = 0;
    // Rows padded past their pixels (a stride wider than the width).
    std::vector<uint32_t> padded(size_t(4 * 2), 0xdead);
    padded[0] = 1, padded[1] = 2, padded[2] = 3, padded[4] = 4, padded[5] = 5, padded[6] = 6;
    const auto out =
        screenshot_upright(reinterpret_cast<const uint8_t*>(padded.data()), 3, 2, 16, WL_OUTPUT_TRANSFORM_NORMAL, &w, &h);
    EXPECT_EQ(w, 3);
    EXPECT_EQ(h, 2);
    EXPECT_EQ(out, (std::vector<uint32_t>{1, 2, 3, 4, 5, 6}));
}

TEST(Screenshot, UpsideDownComesBackTheRightWayUp) {
    int w = 0, h = 0;
    const auto out = upright(numbered(3, 2), 3, 2, WL_OUTPUT_TRANSFORM_180, &w, &h);
    EXPECT_EQ(w, 3);
    EXPECT_EQ(h, 2);
    // The buffer's last pixel is the screen's first.
    EXPECT_EQ(out, (std::vector<uint32_t>{0x12, 0x11, 0x10, 0x02, 0x01, 0x00}));
}

TEST(Screenshot, AMirroredScreenIsMirroredBack) {
    int w = 0, h = 0;
    const auto out = upright(numbered(3, 2), 3, 2, WL_OUTPUT_TRANSFORM_FLIPPED, &w, &h);
    EXPECT_EQ(out, (std::vector<uint32_t>{0x02, 0x01, 0x00, 0x12, 0x11, 0x10}));
}

TEST(Screenshot, ASidewaysScreenComesOutTall) {
    for (auto t : {WL_OUTPUT_TRANSFORM_90, WL_OUTPUT_TRANSFORM_270, WL_OUTPUT_TRANSFORM_FLIPPED_90,
                   WL_OUTPUT_TRANSFORM_FLIPPED_270}) {
        int w = 0, h = 0;
        const auto out = upright(numbered(3, 2), 3, 2, t, &w, &h);
        EXPECT_EQ(w, 2) << t;
        EXPECT_EQ(h, 3) << t;
        // Every pixel once: turned, not smeared.
        std::vector<uint32_t> sorted = out;
        std::ranges::sort(sorted);
        EXPECT_EQ(sorted, (std::vector<uint32_t>{0x00, 0x01, 0x02, 0x10, 0x11, 0x12})) << t;
    }
}

TEST(Screenshot, A90ScreenTurnsItsBufferClockwise) {
    // As a portrait screen at 90 shows it (checked on a headless screen
    // turned that way: the bar along the top).
    int w = 0, h = 0;
    const auto out = upright(numbered(3, 2), 3, 2, WL_OUTPUT_TRANSFORM_90, &w, &h);
    EXPECT_EQ(out, (std::vector<uint32_t>{0x10, 0x00, 0x11, 0x01, 0x12, 0x02}));
    const auto other = upright(numbered(3, 2), 3, 2, WL_OUTPUT_TRANSFORM_270, &w, &h);
    EXPECT_EQ(other, (std::vector<uint32_t>{0x02, 0x12, 0x01, 0x11, 0x00, 0x10}));
}

TEST(Screenshot, TurningBackUndoesTheTurn) {
    // 90 and 270 undo each other: a frame turned one way, then the other,
    // is what it was.
    int w = 0, h = 0, w2 = 0, h2 = 0;
    const auto buf = numbered(4, 3);
    const auto once = upright(buf, 4, 3, WL_OUTPUT_TRANSFORM_90, &w, &h);
    const auto back = upright(once, w, h, WL_OUTPUT_TRANSFORM_270, &w2, &h2);
    EXPECT_EQ(w2, 4);
    EXPECT_EQ(h2, 3);
    EXPECT_EQ(back, buf);
}

} // namespace
