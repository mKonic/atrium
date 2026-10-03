#include "icc.hpp"

#include "util/color.hpp"

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

namespace {

uint32_t be32(const std::vector<uint8_t>& d, size_t at) {
    return uint32_t(d[at]) << 24 | uint32_t(d[at + 1]) << 16 | uint32_t(d[at + 2]) << 8 | uint32_t(d[at + 3]);
}

double s15f16(const std::vector<uint8_t>& d, size_t at) {
    return double(int32_t(be32(d, at))) / 65536.0;
}

// A curve's value at x in [0, 1], its entries spread evenly over it.
float curve_at(const std::vector<float>& c, float x) {
    if (c.empty())
        return x;
    if (c.size() == 1)
        return c[0];
    const float pos = std::clamp(x, 0.0f, 1.0f) * float(c.size() - 1);
    const size_t i = std::min(size_t(pos), c.size() - 2);
    const float t = pos - float(i);
    return c[i] * (1 - t) + c[i + 1] * t;
}

} // namespace

std::optional<Mhc2> parse_mhc2(const std::vector<uint8_t>& d) {
    // 'MHC2', reserved, the curves' length, min and peak luminance, then
    // offsets: the matrix, the red, green and blue curves.
    if (d.size() < 36 || be32(d, 0) != 0x4d484332)
        return std::nullopt;
    Mhc2 m;
    const uint32_t entries = be32(d, 8);
    m.min_nits = s15f16(d, 12);
    m.max_nits = s15f16(d, 16);
    const uint32_t matrix = be32(d, 20);
    const uint32_t curves[3] = {be32(d, 24), be32(d, 28), be32(d, 32)};
    if (matrix) {
        if (size_t(matrix) + 48 > d.size())
            return std::nullopt;
        for (int row = 0; row < 3; ++row)
            for (int col = 0; col < 3; ++col)
                m.xyz[row * 3 + col] = float(s15f16(d, matrix + size_t(row * 4 + col) * 4));
    }
    if (entries) {
        // Each an 'sf32' array: its type, reserved, then the entries.
        std::vector<float>* out[3] = {&m.red, &m.green, &m.blue};
        for (int c = 0; c < 3; ++c) {
            if (!curves[c] || size_t(curves[c]) + 8 + size_t(entries) * 4 > d.size())
                return std::nullopt;
            out[c]->reserve(entries);
            for (uint32_t i = 0; i < entries; ++i)
                out[c]->push_back(float(s15f16(d, curves[c] + 8 + size_t(i) * 4)));
        }
    }
    return m;
}

std::optional<HdrCalibration> load_hdr(const std::string& path, std::string* error, int size) {
    auto fail = [&](const std::string& why) -> std::optional<HdrCalibration> {
        if (error)
            *error = why;
        return std::nullopt;
    };
    Profile p(cmsOpenProfileFromFile(path.c_str(), "r"));
    if (!p)
        return fail("not an ICC profile: " + path);
    const cmsTagSignature sig = cmsTagSignature(0x4d484332);  // 'MHC2'
    const cmsInt32Number n = cmsIsTag(p.get(), sig) ? cmsReadRawTag(p.get(), sig, nullptr, 0) : 0;
    if (n <= 0)
        return fail("no HDR calibration (MHC2) in " + path);
    std::vector<uint8_t> raw(static_cast<size_t>(n));
    cmsReadRawTag(p.get(), sig, raw.data(), cmsUInt32Number(n));
    const auto mhc2 = parse_mhc2(raw);
    if (!mhc2)
        return fail("a damaged HDR calibration (MHC2) in " + path);

    HdrCalibration cal;
    ColorPrimaries bt2020;
    primaries_from_named(&bt2020, NAMED_PRIMARIES_BT2020);
    xyz_matrix_in_primaries(&bt2020, mhc2->xyz, cal.matrix);
    cal.min_nits = mhc2->min_nits;
    cal.max_nits = mhc2->max_nits;
    if (!mhc2->red.empty() && size > 1) {
        cal.lut.size = size;
        cal.lut.rgb.resize(size_t(size) * size * size * 3);
        for (int b = 0; b < size; ++b)
            for (int g = 0; g < size; ++g)
                for (int r = 0; r < size; ++r) {
                    float* out = &cal.lut.rgb[((size_t(b) * size + g) * size + r) * 3];
                    const float k = 1.0f / float(size - 1);
                    out[0] = std::clamp(curve_at(mhc2->red, float(r) * k), 0.0f, 1.0f);
                    out[1] = std::clamp(curve_at(mhc2->green, float(g) * k), 0.0f, 1.0f);
                    out[2] = std::clamp(curve_at(mhc2->blue, float(b) * k), 0.0f, 1.0f);
                }
    }
    char name[256] = {};
    cmsGetProfileInfoASCII(p.get(), cmsInfoDescription, "en", "US", name, sizeof(name));
    cal.description = name;
    cal.lut.description = name;
    return cal;
}

} // namespace atrium::icc
