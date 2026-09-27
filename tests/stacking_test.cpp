#include "stacking.hpp"

#include <gtest/gtest.h>

#include <vector>

using atrium::stacking::in_front_of;
using atrium::stacking::Window;

namespace {

const int kOut1 = 0, kOut2 = 0;
const void* const out1 = &kOut1;
const void* const out2 = &kOut2;
const int kSpace1 = 0, kSpace2 = 0;
const void* const space1 = &kSpace1;
const void* const space2 = &kSpace2;

Window fullscreen() { return {true, true, true, out1, space1}; }
Window normal(const void* output = out1, const void* space = space1) { return {false, true, true, output, space}; }

} // namespace

TEST(Stacking, FullscreenInFrontCoversNothing) {
    std::vector<Window> mru{fullscreen(), normal()};
    EXPECT_EQ(in_front_of(mru, 0), -1);
}

TEST(Stacking, AWindowFocusedAfterItIsInFront) {
    // Alt+Tab or a new app: the other window is now the most recent.
    std::vector<Window> mru{normal(), fullscreen()};
    EXPECT_EQ(in_front_of(mru, 1), 0);
}

TEST(Stacking, TheMostRecentOneIsInFront) {
    std::vector<Window> mru{normal(), normal(), fullscreen()};
    EXPECT_EQ(in_front_of(mru, 2), 0);
}

TEST(Stacking, OtherScreensAndSpacesDontCount) {
    std::vector<Window> mru{normal(out2), normal(out1, space2), fullscreen()};
    EXPECT_EQ(in_front_of(mru, 2), -1);
}

TEST(Stacking, HiddenAndUnmanagedWindowsDontCount) {
    Window minimized = normal();
    minimized.shown = false;
    Window x11_popup = normal();
    x11_popup.managed = false;
    std::vector<Window> mru{minimized, x11_popup, fullscreen()};
    EXPECT_EQ(in_front_of(mru, 2), -1);
}

TEST(Stacking, TwoFullscreenWindowsTheOlderIsCovered) {
    std::vector<Window> mru{fullscreen(), fullscreen()};
    EXPECT_EQ(in_front_of(mru, 0), -1);
    EXPECT_EQ(in_front_of(mru, 1), 0);
}
