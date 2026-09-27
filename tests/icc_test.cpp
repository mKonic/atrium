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
