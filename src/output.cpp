#include "output.hpp"

#include "layer_surface.hpp"
#include "seat.hpp"
#include "server.hpp"
#include "view.hpp"

#include <algorithm>
#include <ctime>

namespace atrium {

Output::Output(Server& srv, wlr_output* output) : server(srv), wlr(output) {
    wlr->data = this;

    wlr_output_state state;
    wlr_output_state_init(&state);
    wlr_output_state_set_mode(&state, wlr_output_preferred_mode(wlr));
    wlr_output_state_set_enabled(&state, true);
    wlr_output_commit_state(wlr, &state);
    wlr_output_state_finish(&state);

    frame_.connect(&wlr->events.frame, [this](void*) { frame(); });
    request_state_.connect(&wlr->events.request_state, [this](wlr_output_event_request_state* e) {
        // The nested backend asks for this when its host window is resized.
        wlr_output_commit_state(e->output, e->state);
        server.update_outputs();
    });
    destroy_.connect(&wlr->events.destroy, [this](void*) { delete this; });

    // xdg-shell: nothing outside a fullscreen surface's own tree may show
    // through it, even where the surface is translucent.
    fullscreen_bg = wlr_scene_rect_create(server.layer(Layer::Fullscreen), 0, 0,
                                          server.config.fullscreen_background.data());
    wlr_scene_node_set_enabled(&fullscreen_bg->node, false);

    scene_output = wlr_scene_output_create(server.scene, wlr);
    // Adding to the layout fires layout.change, which runs update_outputs().
    wlr_output_layout_add_auto(server.output_layout, wlr);
}

Output::~Output() {
    server.animator.cancel_owner(this, true);
    if (!server.shutting_down)
        server.output_removing(this);
    // Layer surfaces cannot outlive their output.
    for (auto& list : layers) {
        auto copy = list;
        for (LayerSurface* l : copy)
            wlr_layer_surface_v1_destroy(l->wlr);
    }
    if (lock_surface) {
        lock_surface_commit.disconnect();
        lock_surface_destroy.disconnect();
        lock_surface = nullptr;
    }

    frame_.disconnect();
    request_state_.disconnect();
    destroy_.disconnect();

    std::erase(server.outputs, this);
    for (View* v : server.views)
        if (v->output == this)
            v->output = nullptr;
    if (server.focused_output == this) {
        server.focused_output = nullptr;
        for (Output* o : server.outputs)
            if (o->enabled()) {
                server.focused_output = o;
                break;
            }
    }

    wlr->data = nullptr;
    wlr_output_layout_remove(server.output_layout, wlr);
    wlr_scene_output_destroy(scene_output);
    wlr_scene_node_destroy(&fullscreen_bg->node);

    if (!server.shutting_down)
        server.update_outputs();
}

void Output::frame() {
    server.animator.tick();
    if (!wlr_scene_output_needs_frame(scene_output))
        return;
    wlr_scene_output_commit(scene_output, nullptr);
    timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    wlr_scene_output_send_frame_done(scene_output, &now);
}

namespace {

void arrange_layer(Output& o, std::vector<LayerSurface*>& list, wlr_box& usable, bool exclusive) {
    const wlr_box full = o.box;
    for (LayerSurface* l : list) {
        if (!l->wlr->initialized)
            continue;
        if (exclusive != (l->wlr->current.exclusive_zone > 0))
            continue;
        wlr_scene_layer_surface_v1_configure(l->scene_layer, &full, &usable);
        wlr_scene_node_set_position(&l->popups->node, l->tree->node.x, l->tree->node.y);
    }
}

} // namespace

void Output::arrange_layers() {
    if (!enabled())
        return;

    wlr_box area = box;
    // Exclusive zones first, top to bottom, so panels shrink the usable area
    // before anything is placed inside it.
    for (int i = 3; i >= 0; --i)
        arrange_layer(*this, layers[i], area, true);

    if (!wlr_box_equal(&area, &usable)) {
        usable = area;
        refit_views();
    }

    for (int i = 3; i >= 0; --i)
        arrange_layer(*this, layers[i], area, false);

    // The topmost mapped surface asking for exclusive keyboard focus gets it.
    for (auto layer : {ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, ZWLR_LAYER_SHELL_V1_LAYER_TOP}) {
        auto& list = layers[layer];
        for (auto it = list.rbegin(); it != list.rend(); ++it) {
            LayerSurface* l = *it;
            if (server.locked || !l->mapped || !l->wants_exclusive_keyboard())
                continue;
            server.focus_layer(l);
            return;
        }
    }
}

void Output::refit_views() {
    for (View* v : server.views) {
        if (v->output != this)
            continue;
        if (v->fullscreen)
            v->request_geometry(box);
        else if (v->maximized)
            v->request_geometry(usable);
    }
    wlr_scene_node_set_enabled(&fullscreen_bg->node,
        std::ranges::any_of(server.views, [this](View* v) {
            return v->output == this && v->fullscreen && v->visible();
        }));
}

} // namespace atrium
