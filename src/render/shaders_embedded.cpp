// The shaders built into atrium: the fallback when a file on disk is
// missing or doesn't compile. Embedded from data/share/atrium/shaders, so
// they are always this tree's own (add a file there and a line here).

#include <cstring>
#include <string_view>

namespace atrium::render {

namespace {

const char k0[] = {
#embed "../../data/share/atrium/shaders/blur1.frag"
    , 0};
const char k1[] = {
#embed "../../data/share/atrium/shaders/blur2.frag"
    , 0};
const char k2[] = {
#embed "../../data/share/atrium/shaders/blur_effects.frag"
    , 0};
const char k3[] = {
#embed "../../data/share/atrium/shaders/box_shadow.frag"
    , 0};
const char k4[] = {
#embed "../../data/share/atrium/shaders/common.vert"
    , 0};
const char k5[] = {
#embed "../../data/share/atrium/shaders/corner_alpha.glsl"
    , 0};
const char k6[] = {
#embed "../../data/share/atrium/shaders/glass.frag"
    , 0};
const char k7[] = {
#embed "../../data/share/atrium/shaders/glass_field.frag"
    , 0};
const char k8[] = {
#embed "../../data/share/atrium/shaders/output.frag"
    , 0};
const char k9[] = {
#embed "../../data/share/atrium/shaders/pq.glsl"
    , 0};
const char k10[] = {
#embed "../../data/share/atrium/shaders/quad.frag"
    , 0};
const char k11[] = {
#embed "../../data/share/atrium/shaders/quad_round.frag"
    , 0};
const char k12[] = {
#embed "../../data/share/atrium/shaders/tex.frag"
    , 0};

struct File {
    std::string_view name;
    const char* text;
};

const File kFiles[] = {
    {"blur1.frag", k0},
    {"blur2.frag", k1},
    {"blur_effects.frag", k2},
    {"box_shadow.frag", k3},
    {"common.vert", k4},
    {"corner_alpha.glsl", k5},
    {"glass.frag", k6},
    {"glass_field.frag", k7},
    {"output.frag", k8},
    {"pq.glsl", k9},
    {"quad.frag", k10},
    {"quad_round.frag", k11},
    {"tex.frag", k12},
};

} // namespace

const char* embedded_shader(std::string_view name) {
    for (const File& f : kFiles)
        if (f.name == name)
            return f.text;
    return nullptr;
}

} // namespace atrium::render
