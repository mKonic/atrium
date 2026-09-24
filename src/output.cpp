#include "output.hpp"

#include "ipc.hpp"
#include "layer_surface.hpp"
#include "night_light.hpp"
#include "overview.hpp"
#include "seat.hpp"
#include "server.hpp"
#include "view.hpp"
#include "geometry.hpp"

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
    server.retile(active);
    wlr_scene_node_set_enabled(&fullscreen_bg->node, false);

    scene_output = wlr_scene_output_create(server.scene, wlr);
    // Adding to the layout fires layout.change, which runs update_outputs().
    wlr_output_layout_add_auto(server.output_layout, wlr);
}

Output::~Output() {
    // Out of the layout and the scene before anything moves off it: a window
    // passing over it would otherwise be told it entered this output, which
    // hooks the wlr_output again while it is being destroyed (wlroots then
    // asserts on the leftover bind listener).
    dying = true;
    wlr_output_layout_remove(server.output_layout, wlr);
    wlr_scene_output_destroy(scene_output);
    scene_output = nullptr;
    server.animator.cancel_owner(this, true);
    if (server.overview)
        server.overview->output_removed(this);
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
    wlr_scene_node_destroy(&fullscreen_bg->node);

    if (!server.shutting_down)
        server.update_outputs();
}

void Output::frame() {
    server.animator.tick();
    // Night light's colour table, when it changed and no app sets this
    // screen's gamma itself; screens without one (nested) do without.
    const NightLight* night = server.night_light.get();
    const bool recolour = night && night_generation_ != night->generation() && wlr_output_get_gamma_size(wlr) > 0 &&
                          !wlr_gamma_control_manager_v1_get_control(server.gamma_manager, wlr);
    // Variable refresh, as set: always, or while a fullscreen game is in front.
    const bool vrr = wlr->adaptive_sync_supported &&
                     (adaptive_sync == "on" || (adaptive_sync == "games" && game_view(server, *this)));
    if (vrr_refused_ && *vrr_refused_ != vrr)
        vrr_refused_.reset();
    const bool switch_vrr = wlr->adaptive_sync_supported &&
                            vrr != (wlr->adaptive_sync_status == WLR_OUTPUT_ADAPTIVE_SYNC_ENABLED) &&
                            vrr_refused_ != vrr;
    // Frame callbacks go out even when nothing changed: a client that asked
    // for one without new damage (Qt between animation steps) would
    // otherwise wait forever, frozen mid-animation.
    if (!wlr_scene_output_needs_frame(scene_output) && !recolour && !switch_vrr) {
        timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        wlr_scene_output_send_frame_done(scene_output, &now);
        return;
    }
    wlr_output_state state;
    wlr_output_state_init(&state);
    if (wlr_scene_output_build_state(scene_output, &state, nullptr)) {
        if (recolour)
            wlr_output_state_set_color_transform(&state, night->transform());
        if (switch_vrr)
            wlr_output_state_set_adaptive_sync_enabled(&state, vrr);
        if (tearing_view(server, *this)) {
            // Show the frame the moment it is ready, torn if need be; fall
            // back to waiting for vblank when the hardware won't.
            state.tearing_page_flip = true;
            if (!wlr_output_test_state(wlr, &state))
                state.tearing_page_flip = false;
        }
        const bool committed = wlr_output_commit_state(wlr, &state);
        if (!committed && (recolour || switch_vrr)) {
            // The screen wouldn't take the table or the switch: the frame
            // without them, and they're not tried again.
            wlr_log(WLR_ERROR, "%s: refused%s%s", wlr->name, recolour ? " night light's colour table" : "",
                    switch_vrr ? " variable refresh" : "");
            state.committed &= ~(WLR_OUTPUT_STATE_COLOR_TRANSFORM | WLR_OUTPUT_STATE_ADAPTIVE_SYNC_ENABLED);
            wlr_output_commit_state(wlr, &state);
            if (switch_vrr)
                vrr_refused_ = vrr;
        }
        if (recolour)
            night_generation_ = night->generation();
        if (committed && switch_vrr && server.ipc)
            server.ipc->broadcast("outputs", {{"event", "outputs.changed"}});
    }
    wlr_output_state_finish(&state);
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
        else if (v->snapped)
            v->request_geometry(geometry::snap_box(usable, v->snapped, server.config.snap_gap));
    }
    wlr_scene_node_set_enabled(&fullscreen_bg->node,
        std::ranges::any_of(server.views, [this](View* v) {
            return v->output == this && v->fullscreen && v->visible();
        }));
}

} // namespace atrium
