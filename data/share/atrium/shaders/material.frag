#version 300 es
// Blur's material (appearance.blur_material), drawn where a window's blur
// shows: the blurred backdrop through frosted ice (refracted along a
// crackled cell pattern) or with a pearly haze. After Hyprland's frost and
// haze finish shaders (BSD-3-Clause, LICENSE.hyprland). The pattern is the
// window's: it moves with it.
precision highp float;

in vec2 v_texcoord;
in vec2 v_frag;
out vec4 frag_color;

uniform sampler2D tex;
uniform float alpha;
// 1: frost, 2: haze.
uniform int material;

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

float hash(vec2 p) {
	vec3 p3 = fract(vec3(p.xyx) * 1689.1984);
	p3 += dot(p3, p3.yzx + 33.33);
	return fract((p3.x + p3.y) * p3.z);
}

// --- frost --------------------------------------------------------------------

const float FROST_CELL = 40.0;       // pixels across a cell
const float FROST_REFRACTION = 20.0; // pixels the backdrop bends at most
const float FROST_ROUGHNESS = 1.0;

vec2 frost_random(vec2 cell) {
	return vec2(hash(cell + vec2(13.37, 71.91)), hash(cell + vec2(83.17, 29.53)));
}

vec2 frost_warp(vec2 p) {
	const vec2 D1 = vec2(1.73, -1.21);
	const vec2 D2 = vec2(1.11, 1.87);
	const vec2 D3 = vec2(-2.19, 0.83);
	vec2 w = vec2(sin(dot(p, D1) + 0.7), cos(dot(p, D2) + 1.9));
	w += vec2(cos(dot(p, D3) + 2.8), sin(dot(p, D1 - D2) + 4.1)) * 0.45;
	return w * 0.16;
}

void frost_cells(vec2 p, out vec2 nearest, out vec2 second) {
	vec2 base = floor(p);
	float nd = 1e10, sd = 1e10;
	nearest = vec2(0.0);
	second = vec2(0.0);
	for (int y = -1; y <= 1; ++y) {
		for (int x = -1; x <= 1; ++x) {
			vec2 cell = base + vec2(float(x), float(y));
			vec2 offset = p - (cell + mix(vec2(0.16), vec2(0.84), frost_random(cell)));
			float d = dot(offset, offset);
			if (d < nd) {
				sd = nd;
				second = nearest;
				nd = d;
				nearest = offset;
			} else if (d < sd) {
				sd = d;
				second = offset;
			}
		}
	}
}

vec2 frost_normal(vec2 p, out float seam) {
	p += frost_warp(p);
	vec2 nearest, second;
	frost_cells(p, nearest, second);
	float boundary = length(second) - length(nearest);
	seam = 1.0 - smoothstep(0.0, 0.028 + fwidth(boundary) * 1.5, boundary);
	vec2 dir = second - nearest;
	dir /= max(length(dir), 0.0001);
	vec2 grain = vec2(sin(dot(p, vec2(2.61, -1.43))), cos(dot(p, vec2(1.19, 2.37)))) * 0.09;
	return nearest * 0.34 + grain + dir * seam * 0.55;
}

vec4 frost() {
	float seam;
	vec2 n = frost_normal((v_frag - position) / FROST_CELL, seam);
	n /= max(1.0, length(n));
	vec2 step_uv = n.x * dFdx(v_texcoord) + n.y * dFdy(v_texcoord);
	vec4 c = texture(tex, clamp(v_texcoord + FROST_REFRACTION * step_uv, vec2(0.0), vec2(1.0)));
	// Lit from above: each cell's relief, and its cracks catching light
	// (stronger than Hyprland's, which sits on lighter backdrops).
	const vec2 LIGHT = vec2(-0.451219, 0.892413);
	c.rgb *= 1.0 + dot(n, LIGHT) * FROST_ROUGHNESS * 0.3;
	c.rgb += vec3(0.07) * seam * c.a;
	return c;
}

// --- haze ---------------------------------------------------------------------

const float HAZE_INTENSITY = 0.35;
const float HAZE_IRIDESCENCE = 0.7;
const vec3 LUMA = vec3(0.2126, 0.7152, 0.0722);

vec3 pearl(float phase) {
	const vec3 COOL = vec3(0.55, 1.08, 1.35);
	const vec3 MID = vec3(1.28, 0.86, 1.30);
	const vec3 WARM = vec3(1.40, 0.92, 0.58);
	float t = phase * 0.5 + 0.5;
	vec3 c = t < 0.5 ? mix(COOL, MID, t * 2.0) : mix(MID, WARM, (t - 0.5) * 2.0);
	return c / max(dot(c, LUMA), 0.001);
}

vec4 haze() {
	vec4 c = texture(tex, v_texcoord);
	if (c.a <= 0.001)
		return c;
	// The frame is gamma 2.2: into light and back.
	vec3 light = pow(max(c.rgb / c.a, vec3(0.0)), vec3(2.2));
	float luminance = max(dot(light, LUMA), 0.0);
	float phase = clamp(dot((v_frag - position) / size - vec2(0.5), vec2(1.28, -0.72)), -1.0, 1.0);
	vec3 shift = luminance * (pearl(phase) - vec3(1.0)) * HAZE_IRIDESCENCE * 0.65;
	vec3 film = max((light + shift) * (1.0 + (1.0 - abs(phase)) * 0.08), vec3(0.0));
	light = mix(light, film, HAZE_INTENSITY);
	vec4 out_c = vec4(pow(max(light, vec3(0.0)), vec3(1.0 / 2.2)) * c.a, c.a);
	// A faint pearly sheen of its own, so dark backdrops show it too.
	out_c.rgb += (pearl(phase) * 0.5 - 0.35) * 0.06 * c.a + 0.02 * c.a;
	return out_c;
}

void main() {
	float quad_alpha = corner_alpha(size - 0.5, position + 0.25, false,
		radius_top_left, radius_top_right, radius_bottom_left, radius_bottom_right);
	float clip_alpha = corner_alpha(clip_size - 1.0, clip_position + 0.5, true,
		clip_radius_top_left, clip_radius_top_right, clip_radius_bottom_left, clip_radius_bottom_right);
	vec4 c = material == 1 ? frost() : material == 2 ? haze() : texture(tex, v_texcoord);
	frag_color = c * alpha * quad_alpha * clip_alpha;
}
