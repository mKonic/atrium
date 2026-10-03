#include "icc.hpp"

#include <gtest/gtest.h>
#include <lcms2.h>

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>

namespace {

// An sRGB-primaries display profile with a pure `gamma`, saved to a file.
std::string write_profile(double gamma, const char* name) {
    const cmsCIExyY white{0.3127, 0.3290, 1.0};
    const cmsCIExyYTRIPLE primaries{{0.64, 0.33, 1.0}, {0.30, 0.60, 1.0}, {0.15, 0.06, 1.0}};
    cmsToneCurve* curve = cmsBuildGamma(nullptr, gamma);
    cmsToneCurve* curves[3] = {curve, curve, curve};
    cmsHPROFILE p = cmsCreateRGBProfile(&white, &primaries, curves);
    cmsFreeToneCurve(curve);
    const std::string path = (std::filesystem::temp_directory_path() / name).string();
    cmsSaveProfileToFile(p, path.c_str());
    cmsCloseProfile(p);
    return path;
}

// The table's value at a grid point.
float at(const atrium::icc::Lut& l, int r, int g, int b, int c) {
    return l.rgb[((size_t(b) * l.size + g) * l.size + r) * 3 + c];
}

} // namespace

TEST(Icc, ProfileLikeTheFrameChangesNothing) {
    const std::string path = write_profile(2.2, "atrium-icc-same.icc");
    std::string error;
    auto lut = atrium::icc::load(path, &error, 17);
    ASSERT_TRUE(lut) << error;
    for (int i = 0; i < 17; i += 4)
        EXPECT_NEAR(at(*lut, i, i, i, 0), i / 16.0, 0.01);
    EXPECT_NEAR(at(*lut, 16, 0, 0, 0), 1.0, 0.01);  // pure red stays pure red
    EXPECT_NEAR(at(*lut, 16, 0, 0, 1), 0.0, 0.01);
    std::filesystem::remove(path);
}

TEST(Icc, AnotherGammaReencodesGrey) {
    // Frame grey 0.5 is 0.5^2.2 of light; a gamma 1.8 display needs that to the 1/1.8.
    const std::string path = write_profile(1.8, "atrium-icc-18.icc");
    auto lut = atrium::icc::load(path, nullptr, 17);
    ASSERT_TRUE(lut);
    EXPECT_NEAR(at(*lut, 8, 8, 8, 1), std::pow(std::pow(0.5, 2.2), 1 / 1.8), 0.01);
    std::filesystem::remove(path);
}

TEST(Icc, NotAProfile) {
    const std::string path = (std::filesystem::temp_directory_path() / "atrium-icc-junk.icc").string();
    std::ofstream(path) << "not a profile";
    std::string error;
    EXPECT_FALSE(atrium::icc::load(path, &error));
    EXPECT_NE(error.find("not an ICC profile"), std::string::npos);
    std::filesystem::remove(path);
}

namespace {

void put32(std::vector<uint8_t>& d, uint32_t v) {
    for (int s = 24; s >= 0; s -= 8)
        d.push_back(uint8_t(v >> s));
}

void put_s15f16(std::vector<uint8_t>& d, double v) {
    put32(d, uint32_t(int32_t(std::lround(v * 65536))));
}

// An MHC2 tag as Microsoft lays it out: header, a 3x4 matrix, three curves.
std::vector<uint8_t> mhc2_tag(const double xyz[9], const std::vector<double>& r, const std::vector<double>& g,
                              const std::vector<double>& b, double min_nits, double max_nits) {
    std::vector<uint8_t> d;
    put32(d, 0x4d484332);  // 'MHC2'
    put32(d, 0);
    put32(d, uint32_t(r.size()));
    put_s15f16(d, min_nits);
    put_s15f16(d, max_nits);
    const uint32_t matrix = 36, curve = 8 + uint32_t(r.size()) * 4;
    put32(d, matrix);
    put32(d, matrix + 48);
    put32(d, matrix + 48 + curve);
    put32(d, matrix + 48 + 2 * curve);
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col)
            put_s15f16(d, xyz[row * 3 + col]);
        put_s15f16(d, 0);  // the offset column
    }
    for (const auto* c : {&r, &g, &b}) {
        put32(d, 0x73663332);  // 'sf32'
        put32(d, 0);
        for (double v : *c)
            put_s15f16(d, v);
    }
    return d;
}

// An HDR display profile carrying `tag` as its MHC2.
std::string write_hdr_profile(const std::vector<uint8_t>& tag, const char* name) {
    const cmsCIExyY white{0.3127, 0.3290, 1.0};
    const cmsCIExyYTRIPLE primaries{{0.708, 0.292, 1.0}, {0.170, 0.797, 1.0}, {0.131, 0.046, 1.0}};
    cmsToneCurve* curve = cmsBuildGamma(nullptr, 1.0);
    cmsToneCurve* curves[3] = {curve, curve, curve};
    cmsHPROFILE p = cmsCreateRGBProfile(&white, &primaries, curves);
    cmsFreeToneCurve(curve);
    if (!tag.empty())
        cmsWriteRawTag(p, cmsTagSignature(0x4d484332), tag.data(), cmsUInt32Number(tag.size()));
    const std::string path = (std::filesystem::temp_directory_path() / name).string();
    cmsSaveProfileToFile(p, path.c_str());
    cmsCloseProfile(p);
    return path;
}

constexpr double kIdentity[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};

} // namespace

TEST(Icc, Mhc2TagParses) {
    const double xyz[9] = {0.9, 0.1, 0, 0, 1, 0, 0, 0.05, 0.95};
    const auto tag = mhc2_tag(xyz, {0, 0.6, 1}, {0, 0.5, 1}, {0.1, 0.4, 0.9}, 0.05, 1015);
    const auto m = atrium::icc::parse_mhc2(tag);
    ASSERT_TRUE(m);
    EXPECT_NEAR(m->min_nits, 0.05, 1e-4);
    EXPECT_NEAR(m->max_nits, 1015, 1e-4);
    for (int i = 0; i < 9; ++i)
        EXPECT_NEAR(m->xyz[i], xyz[i], 1e-4) << i;
    ASSERT_EQ(m->red.size(), 3u);
    EXPECT_NEAR(m->red[1], 0.6, 1e-4);
    EXPECT_NEAR(m->green[1], 0.5, 1e-4);
    EXPECT_NEAR(m->blue[0], 0.1, 1e-4);
}

TEST(Icc, Mhc2DamagedIsRefused) {
    auto tag = mhc2_tag(kIdentity, {0, 1}, {0, 1}, {0, 1}, 0, 1000);
    EXPECT_TRUE(atrium::icc::parse_mhc2(tag));
    tag.resize(tag.size() - 2);  // the blue curve cut short
    EXPECT_FALSE(atrium::icc::parse_mhc2(tag));
    EXPECT_FALSE(atrium::icc::parse_mhc2({1, 2, 3}));
    tag = mhc2_tag(kIdentity, {}, {}, {}, 0, 1000);
    tag[0] = 'X';  // not MHC2
    EXPECT_FALSE(atrium::icc::parse_mhc2(tag));
}

TEST(Icc, HdrCalibrationFromProfile) {
    // Halves all light, and the red signal runs backwards: easy to spot.
    const double half[9] = {0.5, 0, 0, 0, 0.5, 0, 0, 0, 0.5};
    const std::string path =
        write_hdr_profile(mhc2_tag(half, {1, 0}, {0, 1}, {0, 1}, 0.1, 800), "atrium-icc-hdr.icc");
    std::string error;
    const auto cal = atrium::icc::load_hdr(path, &error, 5);
    ASSERT_TRUE(cal) << error;
    // XYZ scaled by a half is BT.2020 scaled by a half.
    for (int i = 0; i < 9; ++i)
        EXPECT_NEAR(cal->matrix[i], i % 4 == 0 ? 0.5 : 0.0, 1e-4) << i;
    EXPECT_NEAR(cal->max_nits, 800, 1e-3);
    EXPECT_NEAR(cal->min_nits, 0.1, 1e-3);
    ASSERT_EQ(cal->lut.size, 5);
    EXPECT_NEAR(at(cal->lut, 0, 0, 0, 0), 1.0, 1e-4);   // red reversed
    EXPECT_NEAR(at(cal->lut, 4, 0, 0, 0), 0.0, 1e-4);
    EXPECT_NEAR(at(cal->lut, 1, 2, 3, 0), 0.75, 1e-4);  // between entries: interpolated
    EXPECT_NEAR(at(cal->lut, 1, 2, 3, 1), 0.5, 1e-4);   // green and blue as they come
    EXPECT_NEAR(at(cal->lut, 1, 2, 3, 2), 0.75, 1e-4);
    std::filesystem::remove(path);
}

TEST(Icc, HdrNeedsMhc2) {
    const std::string plain = write_hdr_profile({}, "atrium-icc-hdr-plain.icc");
    std::string error;
    EXPECT_FALSE(atrium::icc::load_hdr(plain, &error));
    EXPECT_NE(error.find("no HDR calibration"), std::string::npos) << error;
    std::filesystem::remove(plain);

    // A matrix only: no curves, no table.
    const std::string matrix_only =
        write_hdr_profile(mhc2_tag(kIdentity, {}, {}, {}, 0, 0), "atrium-icc-hdr-matrix.icc");
    const auto cal = atrium::icc::load_hdr(matrix_only, &error);
    ASSERT_TRUE(cal) << error;
    EXPECT_EQ(cal->lut.size, 0);
    EXPECT_NEAR(cal->matrix[0], 1, 1e-4);
    std::filesystem::remove(matrix_only);
}
