#include "../src/screencast_core.hpp"

#include <drm_fourcc.h>
#include <spa/param/video/raw.h>

#include <gtest/gtest.h>

using namespace atrium;

namespace {

TEST(ScreenCast, FormatsNameTheSameBytes) {
    // DRM names a little-endian word from its top byte, spa the bytes in
    // memory: the same pixels, opposite names.
    EXPECT_EQ(screencast_spa_format(DRM_FORMAT_XRGB8888), uint32_t(SPA_VIDEO_FORMAT_BGRx));
    EXPECT_EQ(screencast_spa_format(DRM_FORMAT_ARGB8888), uint32_t(SPA_VIDEO_FORMAT_BGRA));
    EXPECT_EQ(screencast_spa_format(DRM_FORMAT_ABGR8888), uint32_t(SPA_VIDEO_FORMAT_RGBA));
    for (uint32_t drm : {DRM_FORMAT_XRGB8888, DRM_FORMAT_ARGB8888, DRM_FORMAT_XBGR8888, DRM_FORMAT_BGRX8888})
        EXPECT_EQ(screencast_drm_format(screencast_spa_format(drm)), drm);
}

TEST(ScreenCast, UnknownFormatsAreSaidSo) {
    EXPECT_EQ(screencast_spa_format(DRM_FORMAT_NV12), uint32_t(SPA_VIDEO_FORMAT_UNKNOWN));
    EXPECT_EQ(screencast_drm_format(SPA_VIDEO_FORMAT_I420), uint32_t(DRM_FORMAT_INVALID));
}

TEST(ScreenCast, ModifiersGoInTheAppsOrder) {
    const std::vector<uint64_t> ours{DRM_FORMAT_MOD_LINEAR, 0x0300000000606012, 0x0300000000606015};
    EXPECT_EQ(screencast_modifiers({0x0300000000606015, DRM_FORMAT_MOD_LINEAR, 0x42}, ours),
              (std::vector<uint64_t>{0x0300000000606015, DRM_FORMAT_MOD_LINEAR}));
}

TEST(ScreenCast, TheImplicitModifierIsALastResort) {
    const std::vector<uint64_t> ours{DRM_FORMAT_MOD_INVALID, DRM_FORMAT_MOD_LINEAR};
    EXPECT_EQ(screencast_modifiers({DRM_FORMAT_MOD_INVALID, DRM_FORMAT_MOD_LINEAR}, ours),
              (std::vector<uint64_t>{DRM_FORMAT_MOD_LINEAR}));
    EXPECT_EQ(screencast_modifiers({DRM_FORMAT_MOD_INVALID}, ours), (std::vector<uint64_t>{DRM_FORMAT_MOD_INVALID}));
}

TEST(ScreenCast, NoModifierInCommonIsNone) {
    EXPECT_TRUE(screencast_modifiers({0x1, 0x2}, {0x3}).empty());
    EXPECT_TRUE(screencast_modifiers({}, {DRM_FORMAT_MOD_LINEAR}).empty());
}

TEST(ScreenCast, RepeatedModifiersCountOnce) {
    // The first value of a spa choice is its default, often repeated.
    EXPECT_EQ(screencast_modifiers({0x5, 0x5, 0x6}, {0x5, 0x6}), (std::vector<uint64_t>{0x5, 0x6}));
}

TEST(ScreenCast, FramesKeepToTheAppsRate) {
    constexpr int64_t ms = 1'000'000;
    // 30 a second: one every 33 ms, a little early allowed.
    EXPECT_EQ(screencast_wait_ns(1000 * ms, 1000 * ms + 33 * ms, 30, 1), 0);
    EXPECT_EQ(screencast_wait_ns(1000 * ms, 1000 * ms + 30 * ms, 30, 1), 0);
    const int64_t wait = screencast_wait_ns(1000 * ms, 1000 * ms + 10 * ms, 30, 1);
    EXPECT_GT(wait, 15 * ms);
    EXPECT_LT(wait, 20 * ms);
}

TEST(ScreenCast, NoRateOrNoFrameYetMeansNow) {
    EXPECT_EQ(screencast_wait_ns(5, 6, 0, 1), 0);
    EXPECT_EQ(screencast_wait_ns(5, 6, 30, 0), 0);
    EXPECT_EQ(screencast_wait_ns(0, 6, 30, 1), 0);
}

} // namespace
