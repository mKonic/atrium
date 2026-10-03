#pragma once
// Colour spaces: named primaries and transfer functions (as bits, so a
// screen can say which it takes), chromaticities, luminances, and a
// screen's gamma ramps as a transform. After wlroots' render/color.c (MIT),
// whose shape atrium's code was written against.
#include <cstddef>
#include <cstdint>

namespace atrium {

enum NamedPrimaries : uint32_t {
    NAMED_PRIMARIES_SRGB = 1 << 0,
    NAMED_PRIMARIES_BT2020 = 1 << 1,
};

enum TransferFunction : uint32_t {
    TRANSFER_FUNCTION_SRGB = 1 << 0,
    TRANSFER_FUNCTION_ST2084_PQ = 1 << 1,
    TRANSFER_FUNCTION_EXT_LINEAR = 1 << 2,
    TRANSFER_FUNCTION_GAMMA22 = 1 << 3,
    TRANSFER_FUNCTION_BT1886 = 1 << 4,
};

struct CieXY {
    float x, y;
};

struct ColorPrimaries {
    CieXY red, green, blue, white;
};

struct Luminances {
    float min, max, reference;
};

void primaries_from_named(ColorPrimaries* out, NamedPrimaries named);
// The 3x3 row-major matrix taking linear `source` RGB to `destination`'s,
// white points kept as they are.
void primaries_transform_absolute_colorimetric(const ColorPrimaries* source, const ColorPrimaries* destination,
                                               float matrix[9]);
// A matrix on CIE XYZ (row-major) as the same change made to `primaries`'
// linear RGB: RGB to XYZ, `xyz`, back to RGB.
void xyz_matrix_in_primaries(const ColorPrimaries* primaries, const float xyz[9], float out[9]);
// A transfer function's default luminances, as the color-management
// protocol defines them.
Luminances default_luminance(TransferFunction tf);

// Per-channel lookup tables (a screen's gamma ramps), referenced.
struct ColorTransform {
    size_t dim = 0;
    uint16_t* lut = nullptr;  // red, green, blue: dim entries each
    size_t n_refs = 1;
};
ColorTransform* color_transform_init_lut_3x1d(size_t dim, const uint16_t* r, const uint16_t* g, const uint16_t* b);
ColorTransform* color_transform_ref(ColorTransform* tr);
void color_transform_unref(ColorTransform* tr);
// `in` (0..1 per channel) through the tables, interpolated.
void color_transform_eval(ColorTransform* tr, float out[3], const float in[3]);

} // namespace atrium
