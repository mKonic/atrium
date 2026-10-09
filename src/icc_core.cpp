#include "icc_core.hpp"

#include <lcms2.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <memory>

namespace atrium {

namespace {

using Mat3 = std::array<double, 9>;  // row major

Mat3 mul(const Mat3& a, const Mat3& b) {
    Mat3 r{};
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            for (int k = 0; k < 3; k++)
                r[i * 3 + j] += a[i * 3 + k] * b[k * 3 + j];
    return r;
}

std::optional<Mat3> invert(const Mat3& m) {
    const double det = m[0] * (m[4] * m[8] - m[5] * m[7]) - m[1] * (m[3] * m[8] - m[5] * m[6]) +
                       m[2] * (m[3] * m[7] - m[4] * m[6]);
    if (std::abs(det) < 1e-12)
        return std::nullopt;
    const double d = 1.0 / det;
    return Mat3{(m[4] * m[8] - m[5] * m[7]) * d, (m[2] * m[7] - m[1] * m[8]) * d, (m[1] * m[5] - m[2] * m[4]) * d,
                (m[5] * m[6] - m[3] * m[8]) * d, (m[0] * m[8] - m[2] * m[6]) * d, (m[2] * m[3] - m[0] * m[5]) * d,
                (m[3] * m[7] - m[4] * m[6]) * d, (m[1] * m[6] - m[0] * m[7]) * d, (m[0] * m[4] - m[1] * m[3]) * d};
}

struct Primaries {
    double rx, ry, gx, gy, bx, by, wx, wy;
};
constexpr Primaries kBT709{0.64, 0.33, 0.30, 0.60, 0.15, 0.06, 0.3127, 0.3290};
constexpr Primaries kBT2020{0.708, 0.292, 0.170, 0.797, 0.131, 0.046, 0.3127, 0.3290};

// RGB (linear) to XYZ, white at Y = 1.
Mat3 to_xyz(const Primaries& p) {
    auto col = [](double x, double y) { return std::array<double, 3>{x / y, 1.0, (1 - x - y) / y}; };
    const auto r = col(p.rx, p.ry), g = col(p.gx, p.gy), b = col(p.bx, p.by), w = col(p.wx, p.wy);
    const Mat3 m{r[0], g[0], b[0], r[1], g[1], b[1], r[2], g[2], b[2]};
    const Mat3 inv = *invert(m);
    const double s[3] = {inv[0] * w[0] + inv[1] * w[1] + inv[2] * w[2], inv[3] * w[0] + inv[4] * w[1] + inv[5] * w[2],
                         inv[6] * w[0] + inv[7] * w[1] + inv[8] * w[2]};
    Mat3 out = m;
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            out[i * 3 + j] *= s[j];
    return out;
}

struct ProfileCloser {
    void operator()(void* h) const { cmsCloseProfile(h); }
};
using Profile = std::unique_ptr<void, ProfileCloser>;

uint32_t be32(const std::vector<unsigned char>& d, size_t at) {
    return uint32_t(d[at]) << 24 | uint32_t(d[at + 1]) << 16 | uint32_t(d[at + 2]) << 8 | d[at + 3];
}
double s15f16(const std::vector<unsigned char>& d, size_t at) {
    return int32_t(be32(d, at)) / 65536.0;
}

// The MHC2 tag (learn.microsoft.com, "Display calibration (MHC2)"), read as
// KWin reads it: its luminances, the matrix (its offsets ignored, as
// Windows does), and the three curves.
struct Mhc2 {
    double min_nits = 0, max_nits = 0;
    Mat3 matrix{1, 0, 0, 0, 1, 0, 0, 0, 1};
    std::vector<float> red, green, blue;
};

std::optional<Mhc2> read_mhc2(cmsHPROFILE h, std::string& error) {
    const auto sig = cmsTagSignature(0x4D484332);  // 'MHC2'
    if (!cmsIsTag(h, sig))
        return std::nullopt;
    const cmsUInt32Number size = cmsReadRawTag(h, sig, nullptr, 0);
    std::vector<unsigned char> d(size);
    if (size < 36 || cmsReadRawTag(h, sig, d.data(), size) != size) {
        error = "its MHC2 tag is smaller than it should be";
        return std::nullopt;
    }
    Mhc2 m;
    const uint32_t lut_size = be32(d, 8);
    m.min_nits = s15f16(d, 12);
    m.max_nits = s15f16(d, 16);
    const uint32_t matrix_at = be32(d, 20), red_at = be32(d, 24), green_at = be32(d, 28), blue_at = be32(d, 32);
    if (matrix_at != 0) {
        if (d.size() < size_t(matrix_at) + 48) {
            error = "its MHC2 tag's matrix is cut short";
            return std::nullopt;
        }
        for (int row = 0; row < 3; row++)
            for (int col = 0; col < 3; col++)
                m.matrix[row * 3 + col] = s15f16(d, matrix_at + (row * 4 + col) * 4);
    }
    if (lut_size > 0) {
        const size_t need = size_t(std::max({red_at, green_at, blue_at})) + 8 + size_t(lut_size) * 4;
        if (d.size() < need) {
            error = "its MHC2 tag's curves are cut short";
            return std::nullopt;
        }
        for (uint32_t i = 0; i < lut_size; i++) {
            m.red.push_back(float(s15f16(d, red_at + 8 + i * 4)));
            m.green.push_back(float(s15f16(d, green_at + 8 + i * 4)));
            m.blue.push_back(float(s15f16(d, blue_at + 8 + i * 4)));
        }
    }
    return m;
}

// The checks KWin makes before it takes a profile.
std::expected<Profile, std::string> open_display_profile(const std::vector<unsigned char>& bytes) {
    Profile p(cmsOpenProfileFromMem(bytes.data(), cmsUInt32Number(bytes.size())));
    if (!p)
        return std::unexpected("it isn't a colour profile");
    if (cmsGetDeviceClass(p.get()) != cmsSigDisplayClass)
        return std::unexpected("it isn't a profile for displays");
    if (cmsGetPCS(p.get()) != cmsSigXYZData)
        return std::unexpected("only profiles that connect through XYZ are supported");
    if (cmsGetColorSpace(p.get()) != cmsSigRgbData)
        return std::unexpected("it isn't an RGB profile");
    return p;
}

// Per-channel curves as a 3D table (exact under trilinear sampling).
template <typename F>
std::vector<float> separable_lut(int n, F&& curve) {
    std::vector<float> lut(size_t(n) * n * n * 3);
    for (int b = 0; b < n; b++)
        for (int g = 0; g < n; g++)
            for (int r = 0; r < n; r++) {
                const float in[3] = {float(r) / (n - 1), float(g) / (n - 1), float(b) / (n - 1)};
                const size_t i = ((size_t(b) * n + g) * n + r) * 3;
                for (int c = 0; c < 3; c++)
                    lut[i + c] = curve(c, in[c]);
            }
    return lut;
}

float eval_curve(const std::vector<float>& table, float x) {
    if (table.empty())
        return x;
    const float pos = std::clamp(x, 0.0f, 1.0f) * float(table.size() - 1);
    const size_t i = size_t(pos);
    if (i + 1 >= table.size())
        return table.back();
    const float t = pos - float(i);
    return table[i] + (table[i + 1] - table[i]) * t;
}

std::array<float, 9> to_float(const Mat3& m) {
    std::array<float, 9> r{};
    for (int i = 0; i < 9; i++)
        r[size_t(i)] = float(m[size_t(i)]);
    return r;
}

// MHC2: its matrix in the screen's linear light, its curves on the signal.
ColorCorrection from_mhc2(const Mhc2& m, const Primaries& wire) {
    ColorCorrection c;
    const Mat3 xyz = to_xyz(wire);
    c.calibration = to_float(mul(*invert(xyz), mul(m.matrix, xyz)));
    if (!m.red.empty()) {
        c.lut_size = kIccLutSize;
        c.lut = separable_lut(kIccLutSize, [&](int ch, float x) {
            return eval_curve(ch == 0 ? m.red : ch == 1 ? m.green : m.blue, x);
        });
    }
    if (m.max_nits > 0)
        c.peak_nits = m.max_nits;
    return c;
}

} // namespace

std::expected<std::vector<unsigned char>, std::string> read_icc(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return std::unexpected("can't open " + path);
    std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (bytes.empty())
        return std::unexpected(path + " is empty");
    return bytes;
}

std::expected<ColorCorrection, std::string> icc_for_sdr(const std::vector<unsigned char>& bytes) {
    auto display = open_display_profile(bytes);
    if (!display)
        return std::unexpected(display.error());
    std::string error;
    if (auto m = read_mhc2(display->get(), error))
        return from_mhc2(*m, kBT709);
    if (!error.empty())
        return std::unexpected(error);

    // What atrium draws: sRGB's primaries at gamma 2.2.
    const cmsCIExyY white{kBT709.wx, kBT709.wy, 1.0};
    const cmsCIExyYTRIPLE primaries{{kBT709.rx, kBT709.ry, 1.0}, {kBT709.gx, kBT709.gy, 1.0}, {kBT709.bx, kBT709.by, 1.0}};
    cmsToneCurve* gamma = cmsBuildGamma(nullptr, 2.2);
    cmsToneCurve* curves[3] = {gamma, gamma, gamma};
    Profile source(cmsCreateRGBProfile(&white, &primaries, curves));
    cmsFreeToneCurve(gamma);
    if (!source)
        return std::unexpected("couldn't build the source profile");
    cmsHTRANSFORM t = cmsCreateTransform(source.get(), TYPE_RGB_FLT, display->get(), TYPE_RGB_FLT, INTENT_PERCEPTUAL,
                                         cmsFLAGS_HIGHRESPRECALC);
    if (!t)
        return std::unexpected("its tables can't be used for this screen");

    const int n = kIccLutSize;
    ColorCorrection c;
    c.lut_size = n;
    c.lut.resize(size_t(n) * n * n * 3);
    for (int b = 0; b < n; b++)
        for (int g = 0; g < n; g++)
            for (int r = 0; r < n; r++) {
                const size_t i = ((size_t(b) * n + g) * n + r) * 3;
                c.lut[i] = float(r) / (n - 1);
                c.lut[i + 1] = float(g) / (n - 1);
                c.lut[i + 2] = float(b) / (n - 1);
            }
    cmsDoTransform(t, c.lut.data(), c.lut.data(), cmsUInt32Number(size_t(n) * n * n));
    cmsDeleteTransform(t);

    // Then the calibration curves loaded into the screen's gamma table on
    // other systems.
    auto** vcgt = static_cast<cmsToneCurve**>(cmsReadTag(display->get(), cmsSigVcgtTag));
    for (size_t i = 0; i < c.lut.size(); i++) {
        float& v = c.lut[i];
        v = std::clamp(v, 0.0f, 1.0f);
        if (vcgt && vcgt[0])
            v = std::clamp(cmsEvalToneCurveFloat(vcgt[i % 3], v), 0.0f, 1.0f);
    }
    return c;
}

std::expected<ColorCorrection, std::string> icc_for_hdr(const std::vector<unsigned char>& bytes) {
    auto display = open_display_profile(bytes);
    if (!display)
        return std::unexpected(display.error());
    std::string error;
    auto m = read_mhc2(display->get(), error);
    if (!m)
        return std::unexpected(error.empty() ? "an HDR calibration is a profile with an MHC2 tag (Windows HDR "
                                               "Calibration and DisplayCAL make them)"
                                             : error);
    return from_mhc2(*m, kBT2020);
}

} // namespace atrium
