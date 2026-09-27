#version 300 es
// After the blur: brightness, contrast, saturation and a little noise.
precision highp float;

in vec2 v_texcoord;
out vec4 frag_color;

uniform sampler2D tex;
uniform float brightness;
uniform float contrast;
uniform float saturation;
uniform float noise;

mat4 brightness_matrix() {
	float b = brightness - 1.0;
	return mat4(1, 0, 0, 0,
		0, 1, 0, 0,
		0, 0, 1, 0,
		b, b, b, 1);
}

mat4 contrast_matrix() {
	float t = (1.0 - contrast) / 2.0;
	return mat4(contrast, 0, 0, 0,
		0, contrast, 0, 0,
		0, 0, contrast, 0,
		t, t, t, 1);
}

mat4 saturation_matrix() {
	vec3 luminance = vec3(0.3086, 0.6094, 0.0820) * (1.0 - saturation);
	vec3 red = vec3(luminance.x);
	red.x += saturation;
	vec3 green = vec3(luminance.y);
	green.y += saturation;
	vec3 blue = vec3(luminance.z);
	blue.z += saturation;
	return mat4(red, 0,
		green, 0,
		blue, 0,
		0, 0, 0, 1);
}

float noise_amount(vec2 p) {
	vec3 p3 = fract(vec3(p.xyx) * 1689.1984);
	p3 += dot(p3, p3.yzx + 33.33);
	float hash = fract((p3.x + p3.y) * p3.z);
	return (mod(hash, 1.0) - 0.5) * noise;
}

void main() {
	vec4 color = texture(tex, v_texcoord);
	// The combined matrix is not transposed on purpose.
	color = brightness_matrix() * contrast_matrix() * saturation_matrix() * color;
	color.xyz += noise_amount(v_texcoord);
	frag_color = color;
}
