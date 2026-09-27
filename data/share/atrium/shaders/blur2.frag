#version 300 es
// Dual Kawase blur, up a level.
precision highp float;

in vec2 v_texcoord;
out vec4 frag_color;

uniform sampler2D tex;
uniform float radius;
uniform vec2 halfpixel;

void main() {
	vec2 uv = v_texcoord / 2.0;
	vec4 sum = texture(tex, uv + vec2(-halfpixel.x * 2.0, 0.0) * radius);
	sum += texture(tex, uv + vec2(-halfpixel.x, halfpixel.y) * radius) * 2.0;
	sum += texture(tex, uv + vec2(0.0, halfpixel.y * 2.0) * radius);
	sum += texture(tex, uv + vec2(halfpixel.x, halfpixel.y) * radius) * 2.0;
	sum += texture(tex, uv + vec2(halfpixel.x * 2.0, 0.0) * radius);
	sum += texture(tex, uv + vec2(halfpixel.x, -halfpixel.y) * radius) * 2.0;
	sum += texture(tex, uv + vec2(0.0, -halfpixel.y * 2.0) * radius);
	sum += texture(tex, uv + vec2(-halfpixel.x, -halfpixel.y) * radius) * 2.0;
	frag_color = sum / 12.0;
}
