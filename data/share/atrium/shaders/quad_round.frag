#version 300 es
// A flat colour with rounded corners, and a rounded cut-out.
precision highp float;

in vec2 v_texcoord;
in vec2 v_frag;
out vec4 frag_color;

uniform vec4 color;
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

void main() {
	float quad_alpha = corner_alpha(size - 1.0, position + 0.5, false,
		radius_top_left, radius_top_right, radius_bottom_left, radius_bottom_right);
	float clip_alpha = corner_alpha(clip_size - 1.0, clip_position + 0.5, true,
		clip_radius_top_left, clip_radius_top_right, clip_radius_bottom_left, clip_radius_bottom_right);
	frag_color = color * quad_alpha * clip_alpha;
}
