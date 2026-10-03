#version 300 es
// A window moving, blurred along its way (after Hyprland's motion_blur.glsl,
// BSD-3-Clause, LICENSE.hyprland): its layer sampled at the places it
// passed through this frame, from where it is now back to where it was, and
// averaged.
precision highp float;

in vec2 v_frag;
out vec4 frag_color;

uniform sampler2D tex;
// A point of the window (0..1 across it) to the layer's texture.
uniform mat3 tex_proj;
// Where it is now, and how far back it was (framebuffer pixels).
uniform vec4 box;
uniform vec2 back;
uniform int samples;
uniform float alpha;

void main() {
	vec4 sum = vec4(0.0);
	for (int i = 0; i < 32; ++i) {
		if (i >= samples)
			break;
		vec2 uv = (v_frag - box.xy - back * (float(i) / float(samples))) / box.zw;
		if (uv.x < 0.0 || uv.y < 0.0 || uv.x > 1.0 || uv.y > 1.0)
			continue;
		sum += texture(tex, (vec3(uv, 1.0) * tex_proj).xy);
	}
	frag_color = sum / float(samples) * alpha;
}
