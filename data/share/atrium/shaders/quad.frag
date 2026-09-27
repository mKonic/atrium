#version 300 es
// A flat colour; EFFECTS: with a rounded cut-out.
precision highp float;

in vec2 v_texcoord;
in vec2 v_frag;
out vec4 frag_color;

uniform vec4 color;

#if EFFECTS
uniform vec2 clip_size;
uniform vec2 clip_position;
uniform float clip_radius_top_left;
uniform float clip_radius_top_right;
uniform float clip_radius_bottom_left;
uniform float clip_radius_bottom_right;

#include "corner_alpha.glsl"
#endif

void main() {
#if EFFECTS
	float clip_alpha = corner_alpha(clip_size - 1.0, clip_position + 0.5, true,
		clip_radius_top_left, clip_radius_top_right, clip_radius_bottom_left, clip_radius_bottom_right);
	frag_color = color * clip_alpha;
#else
	frag_color = color;
#endif
}
