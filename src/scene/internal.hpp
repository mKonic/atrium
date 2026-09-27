#pragma once
// Shared by the scene's sources: walking the tree with its transforms, and
// the region helpers.

#include "scene/scene.hpp"

#include <cmath>

namespace atrium::scene {

// A node's origin in the layout, the scale its own size is drawn at and
// the opacity it's faded by.
struct Walk {
    double x = 0, y = 0;
    double scale = 1;
    float opacity = 1;
    const Tree* warp = nullptr;  // the warped tree it is under, if any

    // Where a tree's child lands.
    Walk child(const Tree* tree, const Node* child) const {
        const double s = scale * tree->scale();
        return {x + child->x * s, y + child->y * s, s, opacity * tree->opacity(), tree->warp() ? tree : warp};
    }
    bool identity() const { return scale == 1.0 && opacity == 1.0f && !warp; }
};

// Float and whole-pixel (rounded out) layout boxes of a node.
render::FBox fbox_of(const Node* node, const Walk& w);
wlr_box box_of(const Node* node, const Walk& w);

using BoxIterator = std::function<bool(Node* node, const Walk& w)>;
// Every enabled leaf node whose box meets `box`, top to bottom; stops when
// the iterator returns true (and returns true).
bool nodes_in_box(Node* node, const wlr_box& box, const BoxIterator& fn);
// The walk of a node (its placement from the root).
Walk walk_of(const Node* node);

// Region helpers (from wlr_scene).
void scale_region(pixman_region32_t* region, float scale, bool round_up);
int scale_length(int length, int offset, float scale);
void scale_box(wlr_box* box, float scale);

struct RenderData {
    wl_output_transform transform;
    float scale;
    wlr_box logical;
    int trans_width, trans_height;
    SceneOutput* output;
    render::RenderPass* pass;
    pixman_region32_t damage;
};

struct Entry {
    Node* node;
    Walk walk;
};

struct OutputAddonAccess {
    static SceneOutput* from(wlr_addon* a);
};

struct SceneImpl {
    static void render_warped(Buffer* b, const Walk& w, RenderData& d, render::RenderPass* pass,
                              wlr_renderer* renderer);
    static void render_entry(const Entry& e, RenderData& d, Scene* scene, render::RenderPass* pass,
                             wlr_renderer* renderer, wlr_drm_syncobj_timeline* in_timeline, uint64_t in_point);
    static void update_outputs(Node* node, wl_list* outputs, SceneOutput* ignore, SceneOutput* force);
    static void output_update(Node* node, wl_list* outputs, SceneOutput* ignore, SceneOutput* force);
    static void update_region(Scene* scene, const pixman_region32_t* region);
    static void damage_outputs(Scene* scene, const pixman_region32_t* damage);
    static void opaque_region(Node* node, const Walk& w, pixman_region32_t* opaque);
    static bool invisible(Node* node);
    static void output_damage(SceneOutput* out, const pixman_region32_t* damage) { out->damage(damage); }
    static void send_frame_done(Node* node, SceneOutput* out, const timespec* now);
    static wlr_texture* texture(Buffer* b, wlr_renderer* r) { return b->texture(r); }
    static bool buffer_black_opaque(const Buffer* b) { return b->is_black_opaque(); }
    static void mark_cache_dirty(Node* node);
};

} // namespace atrium::scene
