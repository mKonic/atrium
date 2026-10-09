#include "cast_core.hpp"

#include <drm_fourcc.h>
#include <gtest/gtest.h>

using namespace atrium::cast;

TEST(Cast, FormatsRoundTrip) {
    EXPECT_EQ(pw_from_drm(DRM_FORMAT_XRGB8888), SPA_VIDEO_FORMAT_BGRx);
    EXPECT_EQ(pw_from_drm(DRM_FORMAT_ABGR8888), SPA_VIDEO_FORMAT_RGBA);
    EXPECT_EQ(pw_from_drm(DRM_FORMAT_XRGB2101010), SPA_VIDEO_FORMAT_xRGB_210LE);
    for (uint32_t f : {DRM_FORMAT_ARGB8888, DRM_FORMAT_XBGR8888, DRM_FORMAT_BGRX1010102, DRM_FORMAT_RGB888})
        EXPECT_EQ(drm_from_pw(pw_from_drm(f)), f);
    EXPECT_EQ(pw_from_drm(DRM_FORMAT_YUYV), SPA_VIDEO_FORMAT_UNKNOWN);
    EXPECT_EQ(drm_from_pw(SPA_VIDEO_FORMAT_I420), DRM_FORMAT_INVALID);
}

TEST(Cast, StripAlpha) {
    EXPECT_EQ(strip_alpha(SPA_VIDEO_FORMAT_BGRA), SPA_VIDEO_FORMAT_BGRx);
    EXPECT_EQ(strip_alpha(SPA_VIDEO_FORMAT_ARGB_210LE), SPA_VIDEO_FORMAT_xRGB_210LE);
    EXPECT_EQ(strip_alpha(SPA_VIDEO_FORMAT_BGRx), SPA_VIDEO_FORMAT_UNKNOWN);
}

TEST(Cast, ShmNamesAndSizes) {
    // wl_shm's two named formats; the rest are fourccs as they are.
    EXPECT_EQ(drm_from_shm(0), DRM_FORMAT_ARGB8888);
    EXPECT_EQ(drm_from_shm(1), DRM_FORMAT_XRGB8888);
    EXPECT_EQ(drm_from_shm(DRM_FORMAT_ABGR8888), DRM_FORMAT_ABGR8888);
    EXPECT_EQ(shm_from_drm(DRM_FORMAT_XRGB8888), 1u);
    EXPECT_EQ(shm_from_drm(DRM_FORMAT_ABGR2101010), DRM_FORMAT_ABGR2101010);
    EXPECT_EQ(bytes_per_pixel(DRM_FORMAT_XRGB8888), 4);
    EXPECT_EQ(bytes_per_pixel(DRM_FORMAT_BGR888), 3);
    EXPECT_EQ(bytes_per_pixel(DRM_FORMAT_NV12), -1);
    EXPECT_EQ(bytes_per_pixel(DRM_FORMAT_YUYV), -1);
}

TEST(Cast, DamageFitsTheSlots) {
    const std::vector<Rect> d{{0, 0, 10, 10}, {50, 50, 10, 10}, {100, 0, 5, 5}, {0, 200, 1, 1}};
    EXPECT_EQ(fit_damage(d, 4), d);
    EXPECT_EQ(fit_damage(d, 16), d);
    const auto two = fit_damage(d, 2);
    ASSERT_EQ(two.size(), 2u);
    EXPECT_EQ(two[0], d[0]);
    EXPECT_EQ(two[1], (Rect{0, 0, 105, 201}));  // the other three folded together
    EXPECT_TRUE(fit_damage(d, 0).empty());
    EXPECT_EQ(merge({10, 10, 5, 5}, {0, 0, 1, 1}), (Rect{0, 0, 15, 15}));
}

TEST(Cast, FramePacing) {
    // 60 a second: a frame every 16.67 ms.
    EXPECT_EQ(frame_delay_ns(60, 0), 16666666u);
    EXPECT_EQ(frame_delay_ns(60, 10'000'000), 6666666u);
    EXPECT_EQ(frame_delay_ns(60, 20'000'000), 0u);
    EXPECT_EQ(frame_delay_ns(0, 0), 0u);  // no limit
}

TEST(Cast, RememberedWindow) {
    const std::vector<Candidate> windows{
        {"a", "org.gnome.Nautilus", "Home"},
        {"b", "firefox", "News — Mozilla Firefox"},
        {"c", "firefox", "Mail — Mozilla Firefox"},
    };
    Choice c;
    c.type = Window;
    c.app_id = "firefox";
    c.title = "Mail — Mozilla Firefox";
    EXPECT_EQ(match_window(c, windows), "c");
    // Its title changed and the app has two windows: which one is unclear.
    c.title = "Inbox — Mozilla Firefox";
    EXPECT_EQ(match_window(c, windows), std::nullopt);
    // The app's only window, whatever it says now.
    c.app_id = "org.gnome.Nautilus";
    EXPECT_EQ(match_window(c, windows), "a");
    c.app_id = "gone";
    EXPECT_EQ(match_window(c, windows), std::nullopt);
    c.app_id.clear();
    EXPECT_EQ(match_window(c, windows), std::nullopt);
}
