#include "layer_surface.hpp"

#include "output.hpp"
#include "palette.hpp"
#include "seat.hpp"
#include "server.hpp"
#include "view.hpp"

#include <algorithm>
#include <cmath>

namespace atrium {

namespace {

// zwlr_layer_shell_v1_layer → scene layer.
Layer scene_layer_for(uint32_t layer) {
    switch (layer) {
    case ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND: return Layer::Background;
    case ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM: return Layer::Bottom;
    case ZWLR_LAYER_SHELL_V1_LAYER_TOP: return Layer::Top;
    default: return Layer::Overlay;
    }
}

} // namespace

LayerSurface::LayerSurface(Server& srv, wlr_layer_surface_v1* surface) : server(srv), wlr(surface) {
    wlr->data = this;
    output = static_cast<Output*>(wlr->output->data);

    wlr_scene_tree* parent = server.layer(scene_layer_for(wlr->pending.layer));
    scene_layer = wlr_scene_layer_surface_v1_create(parent, wlr);
    tree = scene_layer->tree;
    // Popups of background/bottom surfaces (a dock's menu) must still show
    // above windows.
    popups = wlr_scene_tree_create(wlr->pending.layer < ZWLR_LAYER_SHELL_V1_LAYER_TOP
                                       ? server.layer(Layer::Top) : parent);
    wlr->surface->data = popups;  // parent tree for xdg popups
    tree->node.data = popups->node.data = this;

    output->layers[wlr->pending.layer].push_back(this);

    commit_.connect(&wlr->surface->events.commit, [this](void*) { commit(); });
    unmap_.connect(&wlr->surface->events.unmap, [this](void*) { unmap(); });
    destroy_.connect(&wlr->events.destroy, [this](void*) { delete this; });

    wlr_surface_send_enter(wlr->surface, wlr->output);
}

LayerSurface::~LayerSurface() {
    server.animator.cancel_owner(this, false);
    if (output)
        for (auto& list : output->layers)
            std::erase(list, this);
    // `tree` belongs to the scene helper, which frees it on this same destroy
    // signal; only the popup tree is ours.
    wlr_scene_node_destroy(&popups->node);
}

bool LayerSurface::wants_exclusive_keyboard() const {
    return wlr->current.keyboard_interactive == ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE;
}

bool LayerSurface::shown_on_output() const {
    bool shown = false;
    wlr_scene_node_for_each_buffer(&tree->node, [](wlr_scene_buffer* b, int, int, void* data) {
        if (b->primary_output)
            *static_cast<bool*>(data) = true;
    }, &shown);
    return shown;
}

void LayerSurface::commit() {
    if (!output)
        return;
    // The shared background blur is cached; wallpaper and bottom panels
    // changing invalidate it.
    if (wlr->current.layer <= ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM && !wlr->initial_commit)
        wlr_scene_optimized_blur_mark_dirty(server.background_blur);

    if (wlr->initial_commit) {
        float scale = output->wlr->scale;
        wlr_fractional_scale_v1_notify_scale(wlr->surface, scale);
        wlr_surface_set_preferred_buffer_scale(wlr->surface, int32_t(std::ceil(scale)));

        // Arrange as if the pending state were current, so the first configure
        // already has the right size.
        wlr_layer_surface_v1_state old = wlr->current;
        wlr->current = wlr->pending;
        output->arrange_layers();
        wlr->current = old;
        return;
    }

    // Covered entirely (the bar waiting under a fullscreen app), the surface
    // gets no frame callbacks from the scene, and Qt draws nothing, so
    // commits nothing, until it has one: not even the layer change that
    // would bring it over. A frame for its screen sends it one.
    if (wlr->current.layer >= ZWLR_LAYER_SHELL_V1_LAYER_TOP &&
        !wl_list_empty(&wlr->surface->current.frame_callback_list) && !shown_on_output())
        wlr_output_schedule_frame(output->wlr);

    if (wlr->current.committed == 0 && mapped == wlr->surface->mapped)
        return;
    const bool was_mapped = mapped;
    mapped = wlr->surface->mapped;
    // Liquid Glass comes in by bending light more and more, not by fading.
    if (mapped && !was_mapped && server.config.liquid_glass) {
        lensing_ = 0;
        server.animator.cancel_owner(this, false);
        server.animator.start(this, 280, Ease::OutCubic, [this](double t) {
            lensing_ = t;
            update_blur();
        });
    }

    wlr_scene_tree* parent = server.layer(scene_layer_for(wlr->current.layer));
    if (parent != tree->node.parent) {
        wlr_scene_node_reparent(&tree->node, parent);
        for (auto& list : output->layers)
            std::erase(list, this);
        output->layers[wlr->current.layer].push_back(this);
        wlr_scene_node_reparent(&popups->node, wlr->current.layer < ZWLR_LAYER_SHELL_V1_LAYER_TOP
                                                   ? server.layer(Layer::Top) : parent);
    }

    output->arrange_layers();
    update_blur();

    // Top and overlay surfaces asking for exclusive focus get it in
    // arrange_layers. Below windows that is ours to decide: a desktop asking
    // for the keyboard (to rename a file) gets it once, as a click would give
    // it, and gives it back when done.
    const uint32_t ki = wlr->current.keyboard_interactive;
    if (ki != keyboard_interactive_ && wlr->current.layer <= ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM) {
        const bool focused = server.seat->wlr->keyboard_state.focused_surface == wlr->surface;
        if (ki == ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE && mapped)
            server.focus_layer(this);
        else if (ki == ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE && focused)
            server.focus_top();
    }
    keyboard_interactive_ = ki;
}

namespace {

bool namespace_matches(const std::string& ns, const std::vector<std::string>& patterns) {
    for (const std::string& p : patterns) {
        if (!p.empty() && p.back() == '*' ? ns.starts_with(std::string_view(p).substr(0, p.size() - 1)) : ns == p)
            return true;
    }
    return false;
}

wlr_scene_buffer* main_buffer(wlr_scene_tree* tree, wlr_surface* surface) {
    struct Find {
        wlr_surface* surface;
        wlr_scene_buffer* found = nullptr;
    } find{surface};
    wlr_scene_node_for_each_buffer(&tree->node, [](wlr_scene_buffer* b, int, int, void* data) {
        auto* f = static_cast<Find*>(data);
        if (wlr_scene_surface* s = wlr_scene_surface_try_from_buffer(b); s && s->surface == f->surface)
            f->found = b;
    }, &find);
    return find.found;
}

} // namespace

// Liquid Glass (macOS 26, WWDC25 "Meet Liquid Glass"): not frosted but a
// lens. What is behind is only lightly blurred, bent at the edge like a
// rounded bevel, and thicker glass, on a bigger panel, bends it further. It
// takes on just enough of the appearance's colour to keep what is on it
// legible (more where the backdrop fights that), makes the colours behind a
// little richer, and its rim catches a light from the top left. Clear lets
// the most through; Tinted (macOS 26.1) is more opaque.
// How far past its edge Liquid Glass's shadow reaches: its shape blurred by
// a Gaussian of a bevel / 2.5, which fades out by three of those.
constexpr int kGlassShadowReach = 24;

void LayerSurface::update_blur() {
    const Config& c = server.config;
    const bool want = mapped && c.blur && (c.transparency || c.liquid_glass) && wlr->namespace_ &&
                      namespace_matches(wlr->namespace_, c.blurred_panels);
    if (!want) {
        if (blur_)
            wlr_scene_node_set_enabled(&blur_->node, false);
        return;
    }
    wlr_scene_buffer* mask = main_buffer(tree, wlr->surface);
    if (!mask)
        return;
    if (!blur_) {
        blur_ = wlr_scene_blur_create(tree, 0, 0);
        wlr_scene_blur_set_should_only_blur_bottom_layer(blur_, false);  // windows under a bar too
    }
    const int width = wlr->surface->current.width, height = wlr->surface->current.height;
    wlr_scene_node_lower_to_bottom(&blur_->node);
    wlr_scene_node_set_enabled(&blur_->node, true);
    // Liquid Glass casts a soft shadow past the panel's edge: room for it.
    const int reach = c.liquid_glass ? kGlassShadowReach : 0;
    wlr_scene_node_set_position(&blur_->node, mask->node.x - reach, mask->node.y - reach);
    wlr_scene_blur_set_size(blur_, width + 2 * reach, height + 2 * reach);
    // Only where the panel actually draws: a dock's window is mostly empty.
    wlr_scene_blur_set_transparency_mask_source(blur_, mask);
    if (!c.liquid_glass) {
        wlr_scene_blur_set_strength(blur_, 1.0f);
        wlr_scene_blur_set_refraction(blur_, 0, 0);
        return;
    }
    wlr_scene_blur_set_strength(blur_, c.glass_tinted ? 0.45f : 0.12f);
    // The bevel's width, from the panel's short side (a bar is thin glass,
    // Control Center thick), and the slab's height, which sets how far light
    // bends in it: grown in from flat as the glass materializes.
    const float bevel = std::clamp(0.3f * float(std::min(width, height)), 8.0f, 20.0f);
    wlr_scene_blur_set_refraction(blur_, std::max(0.01f, bevel * float(lensing_)), bevel);
    const uint32_t bg = palette::make(c.light, c.accent).window_background;
    wlr_scene_glass glass{};
    glass.tint[0] = float((bg >> 24) & 0xff) / 255;
    glass.tint[1] = float((bg >> 16) & 0xff) / 255;
    glass.tint[2] = float((bg >> 8) & 0xff) / 255;
    glass.tint[3] = c.glass_tinted ? (c.light ? 0.6f : 0.55f) : (c.light ? 0.2f : 0.12f);
    glass.adapt = c.glass_tinted ? 0.2f : 0.3f;
    glass.saturation = c.glass_tinted ? 1.2f : 1.35f;
    glass.highlight = c.light ? 0.6f : 0.5f;
    glass.light_dir[0] = 0.7071f;
    glass.light_dir[1] = 0.7071f;
    glass.shadow = c.light ? 0.16f : 0.3f;
    wlr_scene_blur_set_glass(blur_, &glass);
}

void LayerSurface::unmap() {
    mapped = false;
    if (wlr->current.layer <= ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM)
        wlr_scene_optimized_blur_mark_dirty(server.background_blur);
    wlr_scene_node_set_enabled(&tree->node, false);
    if (wlr->output && (output = static_cast<Output*>(wlr->output->data)))
        output->arrange_layers();
    if (wlr->surface == server.seat->wlr->keyboard_state.focused_surface)
        server.focus_top();
    server.seat->refresh_pointer();
}

} // namespace atrium
