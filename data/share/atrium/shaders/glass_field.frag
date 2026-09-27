#version 300 es
// Liquid Glass's shape field: the panel's coverage, blurred by a Gaussian
// of `sigma` pixels along `dir` (run twice, across then down). Where a
// straight edge is, the field is the normal CDF of the distance to it, so
// glass.frag reads a smooth distance and edge direction from it, and the
// shadow straight off it. The first pass reads the panel's buffer, the second
// the first's output.

precision highp float;

in vec2 v_texcoord;
in vec2 v_frag;
out vec4 frag_color;

uniform sampler2D mask;
uniform bool has_mask;
uniform vec2 box_pos;
uniform vec2 box_size;
uniform vec4 mask_src;
uniform float radius;
uniform sampler2D tex;   // the first pass's output, in the second
uniform vec2 texel;      // one pixel of it
uniform bool first;
uniform vec2 dir;
uniform float sigma;

float cover(vec2 p) {
	if (has_mask) {
		vec2 r = (p - box_pos) / box_size;
		if (r.x < 0.0 || r.y < 0.0 || r.x > 1.0 || r.y > 1.0) {
			return 0.0;
		}
		// Linear in the panel's alpha up to its glass fill's (atrium's shell
		// fills glass at 6%), so the edge's antialiasing carries through.
		return clamp(texture(mask, mask_src.xy + r * mask_src.zw).a * 18.0, 0.0, 1.0);
	}
	vec2 q = abs(p - box_pos - box_size * 0.5) - box_size * 0.5 + radius;
	float d = min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - radius;
	return clamp(0.5 - d, 0.0, 1.0);
}

void main() {
	vec2 p = v_frag;
	float reach = 3.0 * sigma;
	float sum = 0.0;
	float weights = 0.0;
	for (int i = -64; i <= 64; i++) {
		float t = float(i);
		if (abs(t) > reach) {
			continue;
		}
		float w = exp(-0.5 * t * t / (sigma * sigma));
		vec2 q = p + dir * t;
		sum += w * (first ? cover(q) : texture(tex, q * texel).r);
		weights += w;
	}
	frag_color = vec4(sum / weights, 0.0, 0.0, 1.0);
}
