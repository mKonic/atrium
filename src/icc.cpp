#include "icc.hpp"

#include <lcms2.h>

#include <algorithm>
#include <memory>

namespace atrium::icc {

namespace {

// What atrium's frame is: sRGB primaries and white, a pure 2.2 gamma.
cmsHPROFILE frame_profile() {
    const cmsCIExyY white{0.3127, 0.3290, 1.0};
    const cmsCIExyYTRIPLE primaries{{0.64, 0.33, 1.0}, {0.30, 0.60, 1.0}, {0.15, 0.06, 1.0}};
    cmsToneCurve* gamma = cmsBuildGamma(nullptr, 2.2);
    cmsToneCurve* curves[3] = {gamma, gamma, gamma};
    cmsHPROFILE p = cmsCreateRGBProfile(&white, &primaries, curves);
    cmsFreeToneCurve(gamma);
    return p;
}

struct ProfileCloser {
    void operator()(void* p) const { cmsCloseProfile(p); }
};
using Profile = std::unique_ptr<void, ProfileCloser>;

} // namespace

std::optional<Lut> load(const std::string& path, std::string* error, int size) {
    auto fail = [&](const std::string& why) -> std::optional<Lut> {
        if (error)
            *error = why;
        return std::nullopt;
    };
    Profile display(cmsOpenProfileFromFile(path.c_str(), "r"));
    if (!display)
        return fail("not an ICC profile: " + path);
    if (cmsGetColorSpace(display.get()) != cmsSigRgbData)
        return fail("not an RGB profile: " + path);
    Profile frame(frame_profile());
    cmsHTRANSFORM t = cmsCreateTransform(frame.get(), TYPE_RGB_FLT, display.get(), TYPE_RGB_FLT,
                                         INTENT_RELATIVE_COLORIMETRIC, cmsFLAGS_NOCACHE);
    if (!t)
        return fail("can't convert to " + path);

    Lut lut;
    lut.size = size;
    lut.rgb.resize(size_t(size) * size * size * 3);
    std::vector<float> in(size_t(size) * 3);
    for (int b = 0; b < size; ++b)
        for (int g = 0; g < size; ++g) {
            for (int r = 0; r < size; ++r) {
                in[r * 3 + 0] = float(r) / float(size - 1);
                in[r * 3 + 1] = float(g) / float(size - 1);
                in[r * 3 + 2] = float(b) / float(size - 1);
            }
            float* out = &lut.rgb[(size_t(b) * size + g) * size * 3];
            cmsDoTransform(t, in.data(), out, cmsUInt32Number(size));
        }
    cmsDeleteTransform(t);
    for (float& v : lut.rgb)
        v = std::clamp(v, 0.0f, 1.0f);

    char name[256] = {};
    cmsGetProfileInfoASCII(display.get(), cmsInfoDescription, "en", "US", name, sizeof(name));
    lut.description = name;
    return lut;
}

} // namespace atrium::icc
