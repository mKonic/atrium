#version 300 es
// A rounded box's soft shadow, with a rounded cut-out (the window itself).
// Writeup: https://madebyevan.com/shaders/fast-rounded-rectangle-shadows/
precision highp float;

in vec2 v_texcoord;
in vec2 v_frag;
out vec4 frag_color;

uniform vec4 color;
uniform vec2 position;
uniform vec2 size;
uniform float blur_sigma;
uniform float corner_radius;
uniform vec2 clip_position;
uniform vec2 clip_size;
uniform float clip_radius_top_left;
uniform float clip_radius_top_right;
uniform float clip_radius_bottom_left;
uniform float clip_radius_bottom_right;

#include "corner_alpha.glsl"

float gaussian(float x, float sigma) {
	const float pi = 3.141592653589793;
	return exp(-(x * x) / (2.0 * sigma * sigma)) / (sqrt(2.0 * pi) * sigma);
}

// The error function, approximated: the Gaussian's integral.
vec2 erf(vec2 x) {
	vec2 s = sign(x), a = abs(x);
	x = 1.0 + (0.278393 + (0.230389 + 0.078108 * (a * a)) * a) * a;
	x *= x;
	return s - s / (x * x);
}

// The blurred mask along x.
float rounded_box_shadow_x(float x, float y, float sigma, float corner, vec2 half_size) {
	float delta = min(half_size.y - corner - abs(y), 0.0);
	float curved = half_size.x - corner + sqrt(max(0.0, corner * corner - delta * delta));
	vec2 integral = 0.5 + 0.5 * erf((x + vec2(-curved, curved)) * (sqrt(0.5) / sigma));
	return integral.y - integral.x;
}

// The shadow mask of the box from lower to upper.
float rounded_box_shadow(vec2 lower, vec2 upper, vec2 point, float sigma, float corner) {
	vec2 center = (lower + upper) * 0.5;
	vec2 half_size = (upper - lower) * 0.5;
	point -= center;

	// Only non-zero in a limited range: don't waste samples.
	float low = point.y - half_size.y;
	float high = point.y + half_size.y;
	float start = clamp(-3.0 * sigma, low, high);
	float end = clamp(3.0 * sigma, low, high);

	float step = (end - start) / 4.0;
	float y = start + step * 0.5;
	float value = 0.0;
	for (int i = 0; i < 4; i++) {
		value += rounded_box_shadow_x(point.x, point.y - y, sigma, corner, half_size) * gaussian(y, sigma) * step;
		y += step;
	}
	return value;
}

void main() {
	float shadow_alpha = color.a * rounded_box_shadow(position + blur_sigma, position + size - blur_sigma,
		v_frag, blur_sigma * 0.5, corner_radius);
	float clip_alpha = corner_alpha(clip_size - 1.5, clip_position + 0.75, true,
		clip_radius_top_left, clip_radius_top_right, clip_radius_bottom_left, clip_radius_bottom_right);
	frag_color = vec4(color.rgb, shadow_alpha) * clip_alpha;
}
