#include "render/matrix.hpp"

#include <cmath>
#include <cstring>

namespace atrium::render::matrix {

namespace {

constexpr float kTransforms[8][9] = {
    {1, 0, 0, 0, 1, 0, 0, 0, 1},   // NORMAL
    {0, 1, 0, -1, 0, 0, 0, 0, 1},  // 90
    {-1, 0, 0, 0, -1, 0, 0, 0, 1}, // 180
    {0, -1, 0, 1, 0, 0, 0, 0, 1},  // 270
    {-1, 0, 0, 0, 1, 0, 0, 0, 1},  // FLIPPED
    {0, 1, 0, 1, 0, 0, 0, 0, 1},   // FLIPPED_90
    {1, 0, 0, 0, -1, 0, 0, 0, 1},  // FLIPPED_180
    {0, -1, 0, -1, 0, 0, 0, 0, 1}, // FLIPPED_270
};

} // namespace

void identity(float m[9]) {
    static constexpr float id[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    std::memcpy(m, id, sizeof(id));
}

void multiply(float out[9], const float a[9], const float b[9]) {
    float p[9];
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            p[r * 3 + c] = a[r * 3] * b[c] + a[r * 3 + 1] * b[3 + c] + a[r * 3 + 2] * b[6 + c];
    std::memcpy(out, p, sizeof(p));
}

void translate(float m[9], float x, float y) {
    const float t[9] = {1, 0, x, 0, 1, y, 0, 0, 1};
    multiply(m, m, t);
}

void scale(float m[9], float x, float y) {
    const float s[9] = {x, 0, 0, 0, y, 0, 0, 0, 1};
    multiply(m, m, s);
}

void transform(float m[9], wl_output_transform t) {
    multiply(m, m, kTransforms[t & 7]);
}

void projection(float m[9], int width, int height, wl_output_transform tr) {
    std::memset(m, 0, sizeof(float) * 9);
    const float* t = kTransforms[tr & 7];
    const float x = 2.0f / width, y = 2.0f / height;
    m[0] = x * t[0];
    m[1] = x * t[1];
    m[3] = y * -t[3];
    m[4] = y * -t[4];
    m[2] = -std::copysign(1.0f, m[0] + m[1]);
    m[5] = -std::copysign(1.0f, m[3] + m[4]);
    m[8] = 1;
}

bool invert(float out[9], const float m[9]) {
    const float a = m[0], b = m[1], c = m[2], d = m[3], e = m[4], f = m[5], g = m[6], h = m[7], i = m[8];
    const float det = a * e * i + b * f * g + c * d * h - c * e * g - b * d * i - a * f * h;
    if (det == 0)
        return false;
    const float k = 1 / det;
    const float r[9] = {
        k * (e * i - f * h), -k * (b * i - c * h), k * (b * f - c * e),
        -k * (d * i - f * g), k * (a * i - c * g), -k * (a * f - c * d),
        k * (d * h - e * g), -k * (a * h - b * g), k * (a * e - b * d),
    };
    std::memcpy(out, r, sizeof(r));
    return true;
}

void transpose(float out[9], const float in[9]) {
    float t[9];
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            t[c * 3 + r] = in[r * 3 + c];
    std::memcpy(out, t, sizeof(t));
}

bool is_identity(const float m[9]) {
    static constexpr float id[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    return std::memcmp(m, id, sizeof(id)) == 0;
}

} // namespace atrium::render::matrix
