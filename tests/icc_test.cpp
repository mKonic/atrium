#include "../src/icc_core.hpp"

#include <gtest/gtest.h>
#include <lcms2.h>

#include <cmath>
#include <cstdint>

using namespace atrium;

namespace {

// A display profile with sRGB's primaries at `gamma`, as bytes, after
// `edit` has had its way with it.
template <typename Edit>
std::vector<unsigned char> profile(double gamma, Edit&& edit) {
    const cmsCIExyY white{0.3127, 0.3290, 1.0};
    const cmsCIExyYTRIPLE prim{{0.64, 0.33, 1.0}, {0.30, 0.60, 1.0}, {0.15, 0.06, 1.0}};
    cmsToneCurve* g = cmsBuildGamma(nullptr, gamma);
    cmsToneCurve* curves[3] = {g, g, g};
    cmsHPROFILE h = cmsCreateRGBProfile(&white, &prim, curves);
    cmsFreeToneCurve(g);
    edit(h);
    cmsUInt32Number size = 0;
    cmsSaveProfileToMem(h, nullptr, &size);
    std::vector<unsigned char> out(size);
    cmsSaveProfileToMem(h, out.data(), &size);
    cmsCloseProfile(h);
    return out;
}
std::vector<unsigned char> profile(double gamma) {
    return profile(gamma, [](cmsHPROFILE) {});
}

float at(const ColorCorrection& c, int r, int g, int b, int ch) {
    const int n = c.lut_size;
    return c.lut[((size_t(b) * n + g) * n + r) * 3 + ch];
}

void be32(std::vector<unsigned char>& d, size_t at, uint32_t v) {
    d[at] = v >> 24, d[at + 1] = v >> 16, d[at + 2] = v >> 8, d[at + 3] = v;
}
void fixed(std::vector<unsigned char>& d, size_t at, double v) {
    be32(d, at, uint32_t(int32_t(std::lround(v * 65536))));
}

// An MHC2 tag: luminances, a matrix scaling by `scale`, and two-point
// curves from 0 to `top`.
std::vector<unsigned char> mhc2(double min_nits, double max_nits, double scale, double top) {
    std::vector<unsigned char> d(36 + 48 + 3 * (8 + 2 * 4));
    std::copy_n("MHC2", 4, d.begin());
    be32(d, 8, 2);
    fixed(d, 12, min_nits);
    fixed(d, 16, max_nits);
    be32(d, 20, 36);
    for (int row = 0; row < 3; row++)
        fixed(d, 36 + (row * 4 + row) * 4, scale);
    for (int c = 0; c < 3; c++) {
        const uint32_t off = 36 + 48 + c * 16;
        be32(d, 24 + c * 4, off);
        std::copy_n("sf32", 4, d.begin() + off);
        fixed(d, off + 8, 0);
        fixed(d, off + 12, top);
    }
    return d;
}

TEST(Icc, AProfileOfWhatAtriumDrawsChangesNothing) {
    const auto c = icc_for_sdr(profile(2.2));
    ASSERT_TRUE(c) << c.error();
    ASSERT_EQ(c->lut_size, kIccLutSize);
    const int n = c->lut_size;
    for (int i : {0, n / 4, n / 2, n - 1})
        for (int ch = 0; ch < 3; ch++)
            EXPECT_NEAR(at(*c, i, i, i, ch), float(i) / (n - 1), 2e-3);
    EXPECT_NEAR(at(*c, n - 1, 0, 0, 0), 1.0, 2e-3);
    EXPECT_NEAR(at(*c, n - 1, 0, 0, 1), 0.0, 2e-3);
    EXPECT_FALSE(c->calibration);
}

TEST(Icc, AFlatterScreenGetsTheDifference) {
    // A screen at gamma 1.8 shows the same light from x^(2.2/1.8).
    const auto c = icc_for_sdr(profile(1.8));
    ASSERT_TRUE(c) << c.error();
    const int n = c->lut_size, i = n / 2;
    const double x = double(i) / (n - 1);
    EXPECT_NEAR(at(*c, i, i, i, 1), std::pow(x, 2.2 / 1.8), 3e-3);
}

TEST(Icc, ItsCalibrationCurvesComeLast) {
    const auto c = icc_for_sdr(profile(2.2, [](cmsHPROFILE h) {
        cmsToneCurve* half = cmsBuildGamma(nullptr, 1.0);
        cmsFloat32Number pts[2] = {0.0f, 0.5f};
        cmsFreeToneCurve(half);
        half = cmsBuildTabulatedToneCurveFloat(nullptr, 2, pts);
        cmsToneCurve* vcgt[3] = {half, half, half};
        cmsWriteTag(h, cmsSigVcgtTag, vcgt);
        cmsFreeToneCurve(half);
    }));
    ASSERT_TRUE(c) << c.error();
    const int n = c->lut_size;
    EXPECT_NEAR(at(*c, n - 1, n - 1, n - 1, 0), 0.5, 3e-3);
}

TEST(Icc, OnlyDisplayProfilesAreTaken) {
    const auto c = icc_for_sdr(profile(2.2, [](cmsHPROFILE h) { cmsSetDeviceClass(h, cmsSigInputClass); }));
    ASSERT_FALSE(c);
    EXPECT_NE(c.error().find("displays"), std::string::npos);
    EXPECT_FALSE(icc_for_sdr({1, 2, 3}));
}

TEST(Icc, AnMhc2TagIsAMatrixCurvesAndAPeak) {
    const auto tag = mhc2(0.05, 800, 0.5, 0.75);
    const auto bytes = profile(2.2, [&](cmsHPROFILE h) { cmsWriteRawTag(h, cmsTagSignature(0x4D484332), tag.data(), cmsUInt32Number(tag.size())); });
    for (const auto& c : {icc_for_sdr(bytes), icc_for_hdr(bytes)}) {
        ASSERT_TRUE(c) << c.error();
        ASSERT_TRUE(c->calibration);
        // XYZ scaled by a half is the same in any RGB.
        EXPECT_NEAR((*c->calibration)[0], 0.5, 1e-4);
        EXPECT_NEAR((*c->calibration)[1], 0.0, 1e-4);
        EXPECT_NEAR((*c->calibration)[4], 0.5, 1e-4);
        ASSERT_TRUE(c->peak_nits);
        EXPECT_NEAR(*c->peak_nits, 800, 1e-3);
        const int n = c->lut_size;
        ASSERT_GT(n, 0);
        EXPECT_NEAR(at(*c, n - 1, 0, (n - 1) / 2, 0), 0.75, 1e-4);
        EXPECT_NEAR(at(*c, n - 1, 0, (n - 1) / 2, 2), 0.375, 1e-4);
    }
}

TEST(Icc, AnHdrCalibrationNeedsAnMhc2Tag) {
    const auto c = icc_for_hdr(profile(2.2));
    ASSERT_FALSE(c);
    EXPECT_NE(c.error().find("MHC2"), std::string::npos);
}

} // namespace
