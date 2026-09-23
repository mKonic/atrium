#include "layer_surface.hpp"

#include "output.hpp"
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

    if (wlr->current.committed == 0 && mapped == wlr->surface->mapped)
        return;
    mapped = wlr->surface->mapped;

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
