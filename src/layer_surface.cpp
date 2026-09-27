#include "layer_surface.hpp"

#include "glass.hpp"
#include "output.hpp"
#include "palette.hpp"
#include "seat.hpp"
#include "server.hpp"
#include "surface_blur.hpp"
#include "view.hpp"

#include <algorithm>
#include <string_view>
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

    scene::Tree* parent = server.layer(scene_layer_for(wlr->pending.layer));
    scene_layer = scene::layer_surface_v1_create(parent, wlr);
    tree = scene_layer->tree;
    // Popups of background/bottom surfaces (a dock's menu) must still show
    // above windows.
    popups = scene::Tree::create(wlr->pending.layer < ZWLR_LAYER_SHELL_V1_LAYER_TOP
                                       ? server.layer(Layer::Top) : parent);
    wlr->surface->data = popups;  // parent tree for xdg popups
    tree->data = popups->data = this;

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
    popups->destroy();
}

bool LayerSurface::wants_exclusive_keyboard() const {
    return wlr->current.keyboard_interactive == ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE;
}

bool LayerSurface::shown_on_output() const {
    bool shown = false;
    tree->for_each_buffer([&](scene::Buffer* b_, int x_, int y_) { ([](scene::Buffer* b, int, int, void* data) {
        if (b->primary_output)
            *static_cast<bool*>(data) = true;
    })(b_, x_, y_, &shown); });
    return shown;
}

void LayerSurface::commit() {
    if (!output)
        return;
    // The shared background blur is cached; wallpaper and bottom panels
    // changing invalidate it.
    if (wlr->current.layer <= ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM && !wlr->initial_commit)
        server.background_blur->mark_dirty();

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
        server.animator.start(this, 500, Ease::EmphasizedDecel, [this](double t) {
            lensing_ = t;
            update_blur();
        });
    }

    scene::Tree* parent = server.layer(scene_layer_for(wlr->current.layer));
    if (parent != tree->parent) {
        tree->reparent(parent);
        for (auto& list : output->layers)
            std::erase(list, this);
        output->layers[wlr->current.layer].push_back(this);
        popups->reparent(wlr->current.layer < ZWLR_LAYER_SHELL_V1_LAYER_TOP
                                                   ? server.layer(Layer::Top) : parent);
    }

    output->arrange_layers();
    update_blur();

    // The menu bar brought over a fullscreen app (the shell lifts it to the
    // overlay layer): the app's title bar comes out below it, and goes with it.
    if (wlr->namespace_ && std::string_view(wlr->namespace_) == "atrium-bar") {
        const bool over = mapped && wlr->current.layer == ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY;
        const int bottom = tree->y - output->box.y + int(wlr->current.actual_height);
        for (View* v : server.views)
            if (v->output == output && v->fullscreen_front() && v->visible())
                v->reveal_titlebar(over, bottom);
    }

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

} // namespace

// Liquid Glass (macOS 26, WWDC25 "Meet Liquid Glass"): not frosted but a
// lens. What is behind is only lightly blurred, bent at the edge like a
// rounded bevel, and thicker glass, on a bigger panel, bends it further. It
// takes on just enough of the appearance's colour to keep what is on it
// legible (more where the backdrop fights that), makes the colours behind a
// little richer, and its rim catches a light from the top left. Clear lets
// the most through; Tinted (macOS 26.1) is more opaque.

void LayerSurface::update_blur() {
    const Config& c = server.config;
    // Liquid Glass only where the shell said it is (atrium-glass-v1): a
    // surface without shapes (the desktop, the wallpaper) is no glass.
    const auto* given = server.glass_shapes ? server.glass_shapes->shapes_for(wlr->surface) : nullptr;
    const bool is_glass = c.liquid_glass && given && !given->empty();
    const bool want = mapped && c.blur && (c.transparency || is_glass) && wlr->namespace_ &&
                      namespace_matches(wlr->namespace_, c.blurred_panels);
    if (!want) {
        if (blur_)
            blur_->set_enabled(false);
        return;
    }
    scene::Buffer* mask = main_buffer(tree, wlr->surface);
    if (!mask)
        return;
    if (!blur_) {
        blur_ = scene::Blur::create(tree, 0, 0);
        blur_->set_use_cache(false);  // windows under a bar too
    }
    const int width = wlr->surface->current.width, height = wlr->surface->current.height;
    blur_->lower_to_bottom();
    blur_->set_enabled(true);
    // Liquid Glass casts a soft shadow past the panel's edge: room for it.
    const int reach = is_glass ? kGlassShadowReach : 0;
    blur_->set_position(mask->x - reach, mask->y - reach);
    blur_->set_size(width + 2 * reach, height + 2 * reach);
    // Only where the panel actually draws: a dock's window is mostly empty.
    blur_->set_mask(mask);
    if (!is_glass) {
        blur_->set_strength(1.0f);
        blur_->set_refraction(0, 0);
        blur_->set_glass_shapes(nullptr, 0);
        return;
    }
    apply_glass(blur_, *given, float(reach), float(reach), width, height, lensing_, c);
}

void LayerSurface::unmap() {
    mapped = false;
    if (wlr->current.layer <= ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM)
        server.background_blur->mark_dirty();
    tree->set_enabled(false);
    if (wlr->output && (output = static_cast<Output*>(wlr->output->data)))
        output->arrange_layers();
    if (wlr->surface == server.seat->wlr->keyboard_state.focused_surface)
        server.focus_top();
    server.seat->refresh_pointer();
}

} // namespace atrium
