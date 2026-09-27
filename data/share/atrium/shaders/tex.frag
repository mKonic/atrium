#version 300 es
// A texture. SOURCE: 1 RGBA, 2 RGBX, 3 an external (EGLImage) texture.
// EFFECTS: rounded corners and a rounded cut-out.
#define SOURCE_TEXTURE_RGBA 1
#define SOURCE_TEXTURE_RGBX 2
#define SOURCE_TEXTURE_EXTERNAL 3

#if SOURCE == SOURCE_TEXTURE_EXTERNAL
#extension GL_OES_EGL_image_external_essl3 : require
#endif

precision highp float;

in vec2 v_texcoord;
in vec2 v_frag;
out vec4 frag_color;

#if SOURCE == SOURCE_TEXTURE_EXTERNAL
uniform samplerExternalOES tex;
#else
uniform sampler2D tex;
#endif

uniform float alpha;
uniform bool discard_transparent;

#if EFFECTS
uniform vec2 size;
uniform vec2 position;
uniform float radius_top_left;
uniform float radius_top_right;
uniform float radius_bottom_left;
uniform float radius_bottom_right;

uniform vec2 clip_size;
uniform vec2 clip_position;
uniform float clip_radius_top_left;
uniform float clip_radius_top_right;
uniform float clip_radius_bottom_left;
uniform float clip_radius_bottom_right;

#include "corner_alpha.glsl"
#endif

// Content that isn't plain SDR (an HDR video, a game's HDR10 swapchain) is
// decoded to linear light, into sRGB primaries, scaled so 1.0 is SDR white,
// and encoded back to gamma 2.2 like everything else, above 1.0 where it's
// brighter. 0: SDR, drawn as is; 1: PQ; 2: linear; 3: gamma 2.2 in
// another gamut.
uniform int hdr_tf;
uniform mat3 hdr_prim;
uniform float hdr_lum;

#include "pq.glsl"

vec4 hdr_decode(vec4 c) {
	if (hdr_tf == 0) {
		return c;
	}
	vec3 rgb = c.a > 0.0 ? c.rgb / c.a : vec3(0.0);
	if (hdr_tf == 1) {
		rgb = pq_decode(rgb);
	} else if (hdr_tf == 3) {
		rgb = pow(rgb, vec3(2.2));
	}
	rgb = hdr_prim * rgb * hdr_lum;
	rgb = sign(rgb) * pow(abs(rgb), vec3(1.0 / 2.2));
	return vec4(rgb * c.a, c.a);
}

vec4 sample_texture() {
#if SOURCE == SOURCE_TEXTURE_RGBX
	return hdr_decode(vec4(texture(tex, v_texcoord).rgb, 1.0));
#else
	return hdr_decode(texture(tex, v_texcoord));
#endif
}

void main() {
#if EFFECTS
	float quad_alpha = corner_alpha(size - 0.5, position + 0.25, false,
		radius_top_left, radius_top_right, radius_bottom_left, radius_bottom_right);
	float clip_alpha = corner_alpha(clip_size - 1.0, clip_position + 0.5, true,
		clip_radius_top_left, clip_radius_top_right, clip_radius_bottom_left, clip_radius_bottom_right);
	frag_color = sample_texture() * alpha * quad_alpha * clip_alpha;
#else
	frag_color = sample_texture() * alpha;
#endif
	if (discard_transparent && frag_color.a == 0.0) {
		discard;
	}
}
