#include "util/color.hpp"

#include <gtest/gtest.h>

#include <array>

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

TEST(Color, XyzMatrixInPrimaries) {
    // BT.2020 to XYZ, as ITU-R BT.2087 gives it.
    const double t[9] = {0.6370, 0.1446, 0.1689, 0.2627, 0.6780, 0.0593, 0.0, 0.0281, 1.0610};
    // Something lopsided on XYZ: X takes a little of Y, Z loses some.
    const float xyz[9] = {1.0f, 0.1f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.9f};
    ColorPrimaries bt2020;
    primaries_from_named(&bt2020, NAMED_PRIMARIES_BT2020);
    float m[9];
    xyz_matrix_in_primaries(&bt2020, xyz, m);
    // For any RGB: to XYZ, the change, must equal the change in RGB, to XYZ.
    for (const auto& rgb : {std::array<double, 3>{1, 0, 0}, {0.2, 0.7, 0.4}, {0, 0, 1}}) {
        double a[3], b[3], via[3];
        for (int i = 0; i < 3; ++i) {
            a[i] = t[i * 3] * rgb[0] + t[i * 3 + 1] * rgb[1] + t[i * 3 + 2] * rgb[2];
            via[i] = m[i * 3] * rgb[0] + m[i * 3 + 1] * rgb[1] + m[i * 3 + 2] * rgb[2];
        }
        for (int i = 0; i < 3; ++i)
            b[i] = xyz[i * 3] * a[0] + xyz[i * 3 + 1] * a[1] + xyz[i * 3 + 2] * a[2];
        for (int i = 0; i < 3; ++i) {
            const double back = t[i * 3] * via[0] + t[i * 3 + 1] * via[1] + t[i * 3 + 2] * via[2];
            EXPECT_NEAR(back, b[i], 2e-3) << i;
        }
    }
}
