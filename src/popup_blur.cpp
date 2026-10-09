#include "popup_blur.hpp"

#include "server.hpp"

namespace atrium {

namespace {

constexpr float kIgnoreAlpha = 0.2f;  // Hyprland's popups_ignorealpha

wlr_scene_buffer* surface_buffer(wlr_scene_tree* tree, wlr_surface* surface) {
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

void PopupBlur::attach(Server& server, wlr_scene_tree* tree, wlr_surface* surface) {
    new PopupBlur(server, tree, surface);
}

PopupBlur::PopupBlur(Server& server, wlr_scene_tree* tree, wlr_surface* surface)
    : server_(server), tree_(tree), surface_(surface) {
    commit_.connect(&surface->events.commit, [this](void*) { update(); });
    // The tree (and the blur in it) goes with the popup.
    destroy_.connect(&surface->events.destroy, [this](void*) { delete this; });
    update();
}

void PopupBlur::update() {
    const Config& c = server_.config;
    wlr_scene_buffer* mask = surface_->mapped && c.blur && c.transparency ? surface_buffer(tree_, surface_) : nullptr;
    if (!mask) {
        if (blur_)
            wlr_scene_node_set_enabled(&blur_->node, false);
        return;
    }
    if (!blur_) {
        blur_ = wlr_scene_blur_create(tree_, 0, 0);
        wlr_scene_blur_set_should_only_blur_bottom_layer(blur_, false);  // the window under it too
        wlr_scene_blur_set_mask_alpha_threshold(blur_, kIgnoreAlpha);
    }
    int tx = 0, ty = 0, bx = 0, by = 0;
    wlr_scene_node_coords(&tree_->node, &tx, &ty);
    wlr_scene_node_coords(&mask->node, &bx, &by);
    wlr_scene_node_lower_to_bottom(&blur_->node);
    wlr_scene_node_set_enabled(&blur_->node, true);
    wlr_scene_node_set_position(&blur_->node, bx - tx, by - ty);
    wlr_scene_blur_set_size(blur_, surface_->current.width, surface_->current.height);
    wlr_scene_blur_set_transparency_mask_source(blur_, mask);
}

} // namespace atrium
