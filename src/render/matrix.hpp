#pragma once
// 3x3 row-major matrices (wlroots' convention) and the colour constants the
// renderer needs. The transform table and projection come from wlroots
// (MIT), which no longer exports them.

#include "wlr.hpp"

namespace atrium::render::matrix {

void identity(float m[9]);
// out = a * b (out may alias either)
void multiply(float out[9], const float a[9], const float b[9]);
void translate(float m[9], float x, float y);
void scale(float m[9], float x, float y);
void transform(float m[9], wl_output_transform t);
// Pixels of a width x height target to GL clip space.
void projection(float m[9], int width, int height, wl_output_transform t);
bool invert(float out[9], const float m[9]);
void transpose(float out[9], const float in[9]);
bool is_identity(const float m[9]);

} // namespace atrium::render::matrix

namespace atrium::render {

// A transfer function's default luminances (min, max, reference), as the
// color-management protocol defines them.
wlr_color_luminances default_luminance(wlr_color_transfer_function tf);

} // namespace atrium::render
