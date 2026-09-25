#version 440
// Compiled into lens.frag.qsb (committed; the shell loads it as is):
//   qsb --glsl "100 es,120,150" --hlsl 50 --msl 12 -o lens.frag.qsb lens.frag
// A pressed knob as macOS 26 draws it: a capsule of clear glass over the
// track, magnifying what is under it, bending it at the rim (a little apart
// per colour) and catching a thin line of light along its edge.
// `source` is the track around the knob: the knob plus `pad` on every side.

layout(location = 0) in vec2 qt_TexCoord0;
layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform buf {
    mat4 qt_Matrix;
    float qt_Opacity;
    vec2 knob;      // the knob, in pixels
    float pad;      // the source's margin around it, in pixels
    float magnify;  // at the centre
    vec4 tint;      // the glass's own colour (straight alpha)
    vec4 rim;       // the edge's light (straight alpha)
};
layout(binding = 1) uniform sampler2D source;

// Signed distance to a capsule of `knob`, centred at the origin.
float capsule(vec2 p) {
    float r = min(knob.x, knob.y) * 0.5;
    vec2 q = abs(p) - knob * 0.5 + r;
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
}

vec4 sampleAt(vec2 p) {
    // p in knob pixels from its centre -> source texture coordinates.
    vec2 full = knob + 2.0 * pad;
    return texture(source, (p + full * 0.5) / full);
}

void main() {
    vec2 p = (qt_TexCoord0 - 0.5) * knob;
    float d = capsule(p);
    float cover = clamp(0.5 - d, 0.0, 1.0);
    if (cover <= 0.0) {
        fragColor = vec4(0.0);
        return;
    }
    // How far in from the rim (0 at it), over the bevel.
    float bevel = min(knob.x, knob.y) * 0.32;
    float x = clamp(-d / bevel, 0.0, 1.0);
    float edge = pow(1.0 - x, 2.5);
    // Magnified at the centre; near the rim the view is pulled outward, as
    // a lens's curved edge bends it.
    vec2 q = p / magnify;
    vec2 dir = length(p) > 0.001 ? normalize(p) : vec2(0.0);
    vec2 bend = dir * edge * bevel * 0.9;
    vec4 c = sampleAt(q + bend);
    c.r = sampleAt(q + bend * 1.08).r;
    c.b = sampleAt(q + bend * 0.92).b;
    // What's behind through the glass, over its faint tint.
    vec4 col = c + vec4(tint.rgb * tint.a, tint.a) * (1.0 - c.a);
    // The rim: a line a pixel or so wide, brighter toward the top left.
    float line = clamp(1.0 - (-d) / 1.4, 0.0, 1.0);
    float lit = 0.55 + 0.45 * clamp(dot(-dir, normalize(vec2(1.0, 1.0))), -1.0, 1.0);
    col = mix(col, vec4(rim.rgb, 1.0), line * lit * rim.a);
    fragColor = col * cover * qt_Opacity;
}
