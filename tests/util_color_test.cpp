#include "util/color.hpp"

#include <gtest/gtest.h>

using namespace atrium;

namespace {

TEST(Color, SamePrimariesIsIdentity) {
    ColorPrimaries srgb;
    primaries_from_named(&srgb, NAMED_PRIMARIES_SRGB);
    float m[9];
    primaries_transform_absolute_colorimetric(&srgb, &srgb, m);
    const float id[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    for (int i = 0; i < 9; ++i)
        EXPECT_NEAR(m[i], id[i], 1e-5f) << i;
}

TEST(Color, SrgbToBt2020) {
    ColorPrimaries srgb, bt2020;
    primaries_from_named(&srgb, NAMED_PRIMARIES_SRGB);
    primaries_from_named(&bt2020, NAMED_PRIMARIES_BT2020);
    float m[9];
    primaries_transform_absolute_colorimetric(&srgb, &bt2020, m);
    // ITU-R BT.2087's BT.709 → BT.2020 matrix.
    const float want[9] = {0.6274f, 0.3293f, 0.0433f, 0.0691f, 0.9195f, 0.0114f, 0.0164f, 0.0880f, 0.8956f};
    for (int i = 0; i < 9; ++i)
        EXPECT_NEAR(m[i], want[i], 1e-3f) << i;
}

TEST(Color, LutInterpolatesPerChannel) {
    const uint16_t r[] = {0, 65535}, g[] = {65535, 0}, b[] = {0, 0};
    ColorTransform* tr = color_transform_init_lut_3x1d(2, r, g, b);
    const float in[3] = {0.25f, 0.25f, 1.0f};
    float out[3];
    color_transform_eval(tr, out, in);
    EXPECT_NEAR(out[0], 0.25f, 1e-5f);
    EXPECT_NEAR(out[1], 0.75f, 1e-5f);
    EXPECT_NEAR(out[2], 0.0f, 1e-5f);
    EXPECT_EQ(color_transform_ref(tr), tr);
    color_transform_unref(tr);
    color_transform_unref(tr);
}

TEST(Color, DefaultLuminances) {
    EXPECT_EQ(default_luminance(TRANSFER_FUNCTION_ST2084_PQ).reference, 203);
    EXPECT_EQ(default_luminance(TRANSFER_FUNCTION_SRGB).max, 80);
}

} // namespace
