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

#include <sys/timerfd.h>
#include <unistd.h>

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
    // Compositing waits for this timer: just before the screen's next vblank.
    render_timer_fd_ = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (render_timer_fd_ >= 0)
        render_timer_ = wl_event_loop_add_fd(server.loop, render_timer_fd_, WL_EVENT_READABLE,
            [](int fd, uint32_t, void* data) {
                uint64_t expirations;
                if (read(fd, &expirations, sizeof expirations) > 0)
                    static_cast<Output*>(data)->render();
                return 0;
            }, this);
    request_state_.connect(&wlr->events.request_state, [this](wlr_output_event_request_state* e) {
        // The nested backend asks for this when its host window is resized.
        wlr_output_commit_state(e->output, e->state);
        server.update_outputs();
    });
    destroy_.connect(&wlr->events.destroy, [this](void*) { delete this; });
    present_.connect(&wlr->events.present, [this](wlr_output_event_present* e) {
        if (!e->presented)
            return;
        presented(e->when.tv_sec * 1000000000LL + e->when.tv_nsec, e->refresh);
    });

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
    present_.disconnect();
    if (render_timer_)
        wl_event_source_remove(render_timer_);
    if (render_timer_fd_ >= 0)
        close(render_timer_fd_);
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

namespace {

int64_t now_ns() {
    timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000000000LL + t.tv_nsec;
}

} // namespace

// The screen is ready for a frame. Composited right away, a window's new
// frame that arrives a moment later waits for the next vblank to be drawn
// and another to be shown; composited just before the vblank, it is shown
// at that one. So on a real screen with a fixed refresh the frame waits
// for the vblank after next minus a margin: what compositing takes, learnt
// from frames that missed their vblank. Variable refresh and tearing show
// a frame the moment it's committed, so they don't wait.
void Output::frame() {
    if (render_timer_ && vblank_ns_ && period_ns_ && !wlr_output_is_wl(wlr) && !wlr_output_is_headless(wlr) &&
        wlr->adaptive_sync_status != WLR_OUTPUT_ADAPTIVE_SYNC_ENABLED && !tearing_view(server, *this)) {
        const int64_t now = now_ns();
        const int64_t next = vblank_ns_ + ((now - vblank_ns_) / period_ns_ + 1) * period_ns_;
        const int64_t start = next - margin_ns_;
        if (start - now > kMinDelayNs) {
            aimed_ns_ = next;
            itimerspec at{};
            at.it_value.tv_sec = start / 1000000000LL;
            at.it_value.tv_nsec = start % 1000000000LL;
            if (timerfd_settime(render_timer_fd_, TFD_TIMER_ABSTIME, &at, nullptr) == 0)
                return;
        }
    }
    aimed_ns_ = 0;
    render();
}

// Where the screen's vblanks fall, and whether the last frame made the one
// it was aimed at: a miss widens the margin at once, a long run of hits
// narrows it again.
void Output::presented(int64_t when, int refresh) {
    if (refresh > 0)
        period_ns_ = refresh;
    else if (wlr->refresh > 0)
        period_ns_ = 1000000000000LL / wlr->refresh;
    vblank_ns_ = when;
    if (!aimed_ns_ || !period_ns_)
        return;
    if (when > aimed_ns_ + period_ns_ / 2) {
        margin_ns_ = std::min(margin_ns_ + kMarginStepNs * 4, period_ns_ / 2);
        on_time_ = 0;
    } else if (++on_time_ >= kOnTimeToNarrow) {
        margin_ns_ = std::max(margin_ns_ - kMarginStepNs, kMinMarginNs);
        on_time_ = 0;
    }
    aimed_ns_ = 0;
}

void Output::render() {
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
        send_frame_done();
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
    send_frame_done();
}

// The scene sends frame callbacks to what shows; panels covered by a
// fullscreen app get theirs here, or they could never draw themselves back
// over it (see LayerSurface::commit).
void Output::send_frame_done() {
    timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    wlr_scene_output_send_frame_done(scene_output, &now);
    // Top and overlay only: a wallpaper animating under a window stays paused.
    for (auto layer : {ZWLR_LAYER_SHELL_V1_LAYER_TOP, ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY})
        for (LayerSurface* l : layers[layer])
            if (l->mapped)
                wlr_surface_send_frame_done(l->wlr->surface, &now);
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
