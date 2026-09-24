#include "capture_core.hpp"

#include <gtest/gtest.h>

using namespace atrium::capture;

TEST(Capture, FileName) {
    std::tm t{};
    t.tm_year = 126;
    t.tm_mon = 8;
    t.tm_mday = 4;
    t.tm_hour = 9;
    t.tm_min = 5;
    t.tm_sec = 7;
    EXPECT_EQ(file_name(t), "Screenshot 2026-09-04 at 09.05.07.png");
}

TEST(Capture, Topmost) {
    const std::vector<Box> boxes = {{100, 100, 200, 100}, {0, 0, 400, 400}};
    EXPECT_EQ(topmost_at(boxes, 150, 150), 0);
    EXPECT_EQ(topmost_at(boxes, 50, 50), 1);
    EXPECT_EQ(topmost_at(boxes, 300, 150), 1);   // right edge is outside the first
    EXPECT_EQ(topmost_at(boxes, 400, 10), -1);
    EXPECT_EQ(topmost_at({}, 0, 0), -1);
}

TEST(Capture, ToPixels) {
    // A 1.5x screen: 1280x800 logical, 1920x1200 pixels.
    EXPECT_EQ(to_pixels({10, 10, 101, 51}, 1280, 800, 1920, 1200), (Box{15, 15, 152, 77}));
    EXPECT_EQ(to_pixels({0, 0, 1280, 800}, 1280, 800, 1920, 1200), (Box{0, 0, 1920, 1200}));
    // Past the edge: cut to the picture.
    EXPECT_EQ(to_pixels({1200, 700, 200, 200}, 1280, 800, 1280, 800), (Box{1200, 700, 80, 100}));
    EXPECT_EQ(to_pixels({0, 0, 10, 10}, 0, 0, 100, 100), (Box{}));
}

TEST(Capture, Between) {
    EXPECT_EQ(between(50, 60, 10, 20), (Box{10, 20, 40, 40}));
    EXPECT_EQ(between(10, 20, 50, 60), (Box{10, 20, 40, 40}));
}
