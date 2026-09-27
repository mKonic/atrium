#version 300 es
// Every program's vertex shader: a unit quad mapped onto its box.
precision highp float;

uniform mat3 proj;
uniform mat3 tex_proj;
// The framebuffer's size, for v_frag.
uniform vec2 frag_size;

in vec2 pos;
#ifdef MESH
// A warped piece: `pos` is the vertex's place in the piece (0..1, for the
// texture) and `at` where it lands, in framebuffer pixels.
in vec2 at;
#endif
out vec2 v_texcoord;
// Where a fragment is, in framebuffer pixels: gl_FragCoord, but highp.
// NVIDIA's GLES has no GL_FRAGMENT_PRECISION_HIGH and its mediump is real
// fp16, so past 1024 pixels gl_FragCoord rounds pixel centres in pairs.
out vec2 v_frag;

void main() {
	vec3 pos3 = vec3(pos, 1.0);
#ifdef MESH
	gl_Position = vec4(vec3(at, 1.0) * proj, 1.0);
#else
	gl_Position = vec4(pos3 * proj, 1.0);
#endif
	v_texcoord = (pos3 * tex_proj).xy;
	v_frag = (gl_Position.xy * 0.5 + 0.5) * frag_size;
}
