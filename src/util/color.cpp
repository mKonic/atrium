#include "util/color.hpp"

#include <cassert>
#include <cmath>
#include <cstring>

namespace atrium {

namespace {

constexpr ColorPrimaries kSrgb = {{0.640f, 0.330f}, {0.300f, 0.600f}, {0.150f, 0.060f}, {0.3127f, 0.3290f}};
constexpr ColorPrimaries kBt2020 = {{0.708f, 0.292f}, {0.170f, 0.797f}, {0.131f, 0.046f}, {0.3127f, 0.3290f}};

void mul_vec(float out[3], const float m[9], const float v[3]) {
    const float r[3] = {m[0] * v[0] + m[1] * v[1] + m[2] * v[2], m[3] * v[0] + m[4] * v[1] + m[5] * v[2],
                        m[6] * v[0] + m[7] * v[1] + m[8] * v[2]};
    std::memcpy(out, r, sizeof r);
}

void mul(float out[9], const float a[9], const float b[9]) {
    float r[9];
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            r[i * 3 + j] = a[i * 3] * b[j] + a[i * 3 + 1] * b[3 + j] + a[i * 3 + 2] * b[6 + j];
    std::memcpy(out, r, sizeof r);
}

void invert(float out[9], const float m[9]) {
    const float a = m[0], b = m[1], c = m[2], d = m[3], e = m[4], f = m[5], g = m[6], h = m[7], i = m[8];
    const float det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    assert(det != 0);
    const float r[9] = {(e * i - f * h) / det, (c * h - b * i) / det, (b * f - c * e) / det,
                        (f * g - d * i) / det, (a * i - c * g) / det, (c * d - a * f) / det,
                        (d * h - e * g) / det, (b * g - a * h) / det, (a * e - b * d) / det};
    std::memcpy(out, r, sizeof r);
}

void xy_to_xyz(float out[3], CieXY p) {
    if (p.y == 0) {
        out[0] = out[1] = out[2] = 0;
        return;
    }
    out[0] = p.x / p.y;
    out[1] = 1;
    out[2] = (1 - p.x - p.y) / p.y;
}

// RGB to XYZ (brucelindbloom.com, "RGB/XYZ matrices").
void to_xyz(const ColorPrimaries* p, float m[9]) {
    float r[3], g[3], b[3], w[3];
    xy_to_xyz(r, p->red);
    xy_to_xyz(g, p->green);
    xy_to_xyz(b, p->blue);
    xy_to_xyz(w, p->white);
    float xyz[9] = {r[0], g[0], b[0], r[1], g[1], b[1], r[2], g[2], b[2]};
    invert(xyz, xyz);
    float s[3];
    mul_vec(s, xyz, w);
    const float out[9] = {s[0] * r[0], s[1] * g[0], s[2] * b[0], s[0] * r[1], s[1] * g[1],
                          s[2] * b[1], s[0] * r[2], s[1] * g[2], s[2] * b[2]};
    std::memcpy(m, out, sizeof out);
}

float lut_get(const uint16_t* lut, size_t len, size_t i) { return float(lut[i < len ? i : len - 1]) / UINT16_MAX; }

float lut_eval(const uint16_t* lut, size_t len, float x) {
    double whole;
    const double frac = std::modf(double(x) * double(len - 1), &whole);
    const size_t i = size_t(whole);
    return float(lut_get(lut, len, i) * (1 - frac) + lut_get(lut, len, i + 1) * frac);
}

} // namespace

void primaries_from_named(ColorPrimaries* out, NamedPrimaries named) {
    *out = named == NAMED_PRIMARIES_BT2020 ? kBt2020 : kSrgb;
}

void primaries_transform_absolute_colorimetric(const ColorPrimaries* source, const ColorPrimaries* destination,
                                               float matrix[9]) {
    float src[9], dst[9];
    to_xyz(source, src);
    to_xyz(destination, dst);
    invert(dst, dst);
    mul(matrix, dst, src);
}

void xyz_matrix_in_primaries(const ColorPrimaries* primaries, const float xyz[9], float out[9]) {
    float to[9], from[9], m[9];
    to_xyz(primaries, to);
    invert(from, to);
    mul(m, xyz, to);
    mul(out, from, m);
}

Luminances default_luminance(TransferFunction tf) {
    switch (tf) {
    case TRANSFER_FUNCTION_ST2084_PQ:
        return {0.005f, 10000, 203};
    case TRANSFER_FUNCTION_BT1886:
        return {0.01f, 100, 100};
    default:
        return {0.2f, 80, 80};
    }
}

ColorTransform* color_transform_init_lut_3x1d(size_t dim, const uint16_t* r, const uint16_t* g, const uint16_t* b) {
    assert(dim > 0);
    auto* tr = new ColorTransform;
    tr->dim = dim;
    tr->lut = new uint16_t[3 * dim];
    std::memcpy(tr->lut, r, dim * sizeof *r);
    std::memcpy(tr->lut + dim, g, dim * sizeof *g);
    std::memcpy(tr->lut + 2 * dim, b, dim * sizeof *b);
    return tr;
}

ColorTransform* color_transform_ref(ColorTransform* tr) {
    ++tr->n_refs;
    return tr;
}

void color_transform_unref(ColorTransform* tr) {
    if (!tr)
        return;
    assert(tr->n_refs > 0);
    if (--tr->n_refs > 0)
        return;
    delete[] tr->lut;
    delete tr;
}

void color_transform_eval(ColorTransform* tr, float out[3], const float in[3]) {
    for (size_t c = 0; c < 3; ++c)
        out[c] = lut_eval(tr->lut + tr->dim * c, tr->dim, in[c]);
}

} // namespace atrium
