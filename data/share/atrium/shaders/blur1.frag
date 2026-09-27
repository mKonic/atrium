#version 300 es
// Dual Kawase blur, down a level.
precision highp float;

in vec2 v_texcoord;
out vec4 frag_color;

uniform sampler2D tex;
uniform float radius;
uniform vec2 halfpixel;

void main() {
	vec2 uv = v_texcoord * 2.0;
	vec4 sum = texture(tex, uv) * 4.0;
	sum += texture(tex, uv - halfpixel.xy * radius);
	sum += texture(tex, uv + halfpixel.xy * radius);
	sum += texture(tex, uv + vec2(halfpixel.x, -halfpixel.y) * radius);
	sum += texture(tex, uv - vec2(halfpixel.x, -halfpixel.y) * radius);
	frag_color = sum / 8.0;
}
