#version 300 es
// The colour output pass: the blend buffer holds the frame gamma-encoded as
// always (1.0 = SDR white; HDR content goes above it), and this turns it
// into the screen's signal: linear light, into its primaries at its
// luminance, then its transfer function (PQ for HDR10).
precision highp float;

in vec2 v_texcoord;
out vec4 frag_color;

uniform sampler2D tex;
uniform mat3 matrix;
// 0: gamma 2.2, 1: PQ, 2: linear, 3: sRGB
uniform int out_tf;
// The display's colour profile (SDR): the encoded frame looked up in it.
uniform int has_lut;
uniform highp sampler3D lut;
uniform float lut_size;

#include "pq.glsl"

vec3 srgb_encode(vec3 l) {
	vec3 lo = l * 12.92;
	vec3 hi = 1.055 * pow(l, vec3(1.0 / 2.4)) - 0.055;
	return mix(lo, hi, step(vec3(0.0031308), l));
}

void main() {
	vec3 c = texture(tex, v_texcoord).rgb;
	vec3 lin = sign(c) * pow(abs(c), vec3(2.2));
	lin = max(matrix * lin, 0.0);
	vec3 e;
	if (out_tf == 1) {
		e = pq_encode(lin);
	} else if (out_tf == 2) {
		e = lin;
	} else if (out_tf == 3) {
		e = srgb_encode(min(lin, 1.0));
	} else {
		e = pow(min(lin, 1.0), vec3(1.0 / 2.2));
	}
	if (has_lut == 1 && (out_tf == 0 || out_tf == 3))
		e = texture(lut, e * ((lut_size - 1.0) / lut_size) + 0.5 / lut_size).rgb;
	frag_color = vec4(e, 1.0);
}
