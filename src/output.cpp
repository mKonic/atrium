#include "output.hpp"
#include "util/log.hpp"
#include "icc.hpp"
#include "render/renderer.hpp"

#include "ipc.hpp"
#include "layer_surface.hpp"
#include "night_light.hpp"
#include "overview.hpp"
#include "seat.hpp"
#include "server.hpp"
#include "view.hpp"
#include "geometry.hpp"

#include <algorithm>
#include <drm_fourcc.h>
#include <xf86drm.h>
#include <ctime>

#include <sys/timerfd.h>
#include <unistd.h>

namespace atrium {

Output::Output(Server& srv, backend::Output* output) : server(srv), screen(output) {
    screen->data = this;

    backend::OutputState state;
    if (const backend::Mode* m = screen->preferred_mode())
        state.set_mode(m);
    state.set_enabled(true);
    screen->commit_state(state);

    frame_ = screen->events.frame.connect([this] { frame(); });
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
    request_state_ = screen->events.request_state.connect([this](const backend::OutputState& st) {
        // The nested backend asks for this when its host window is resized.
        screen->commit_state(st);
        server.update_outputs();
    });
    destroy_ = screen->events.destroy.connect([this] { delete this; });
    present_ = screen->events.present.connect([this](const backend::Present& e) {
        if (!e.presented)
            return;
        presented(e.when.tv_sec * 1000000000LL + e.when.tv_nsec, e.refresh);
    });

    // xdg-shell: nothing outside a fullscreen surface's own tree may show
    // through it, even where the surface is translucent.
    fullscreen_bg = scene::Rect::create(server.layer(Layer::Fullscreen), 0, 0, server.config.fullscreen_background.data());
    server.retile(active);
    fullscreen_bg->set_enabled(false);

    global = std::make_unique<wl::Output>(server.display, wl::OutputInfo{});
    global->data = this;
    sync_global();
    scene_output = scene::SceneOutput::create(server.scene, screen);
    scene_output->global = global.get();
    server.wl->gamma->set_size(global.get(), uint32_t(screen->gamma_size()));
    if (screen->backend.is_drm() && !server.backend->is_virtual(screen))
        hdr_caps = hdr_caps_from_edid(connector_edid(screen->name));
    // Adding to the layout fires layout.change, which runs update_outputs().
    server.output_layout->add_auto(screen);
}

Output::~Output() {
    // Out of the layout and the scene before anything moves off it: a window
    // passing over it would otherwise be told it entered this output, which
    // hooks the backend::Output again while it is being destroyed (wlroots then
    // asserts on the leftover bind listener).
    dying = true;
    server.output_layout->remove(screen);
    scene_output->destroy();
    scene_output = nullptr;
    server.animator.cancel_owner(this, true);
    server.capture_output_gone(this);
    if (server.overview)
        server.overview->output_removed(this);
    if (!server.shutting_down)
        server.output_removing(this);
    // Layer surfaces cannot outlive their output.
    for (auto& list : layers) {
        auto copy = list;
        for (LayerSurface* l : copy)
            l->ls->close();  // its destroy event takes ours with it
    }
    server.wl->gamma->set_size(global.get(), 0);
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
    if (render_fence_ >= 0)
        close(render_fence_);
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

    screen->data = nullptr;
    // Nothing may name the wl_output past here.
    server.wl->toplevels->remove_output(global.get());
    server.wl->workspaces->remove_output(global.get());
    global->data = nullptr;
    global.reset();
    fullscreen_bg->destroy();

    if (!server.shutting_down)
        server.update_outputs();
}

// What clients learn of the screen, as it is now.
void Output::sync_global() {
    if (!global)
        return;
    wl::OutputInfo i;
    i.name = screen->name;
    i.description = screen->description;
    i.make = screen->make;
    i.model = screen->model;
    i.physical_width = screen->phys_width;
    i.physical_height = screen->phys_height;
    i.subpixel = int32_t(screen->subpixel);
    i.transform = int32_t(screen->transform);
    i.scale = screen->scale;
    i.mode_width = screen->width;
    i.mode_height = screen->height;
    i.refresh = screen->refresh;
    i.x = box.x;
    i.y = box.y;
    int w = 0, h = 0;
    screen->effective_resolution(&w, &h);
    i.logical_width = w;
    i.logical_height = h;
    global->update(i);
    // What content suits it: HDR10 while it shows HDR, else sRGB.
    wl::ImageDescription d;
    d.tf_named = hdr_active() ? 11 : 2;
    d.primaries_named = hdr_active() ? 6 : 1;
    server.wl->color->set_output_description(global.get(), d);
}

bool Output::hdr_supported() const {
    return (screen->supported_transfer_functions & TRANSFER_FUNCTION_ST2084_PQ) &&
           (screen->supported_primaries & NAMED_PRIMARIES_BT2020);
}

bool Output::hdr_active() const {
    return screen->image_description && screen->image_description->transfer_function == TRANSFER_FUNCTION_ST2084_PQ;
}

namespace {

std::string format_name(uint32_t format) {
    char* n = drmGetFormatName(format);
    std::string out = n ? n : "?";
    free(n);
    return out;
}

// Why no 10-bit format was taken: what the screen's plane offers.
void log_formats(backend::Output* output) {
    const FormatSet* formats = output->primary_formats(BUFFER_CAP_DMABUF);
    std::string list;
    if (formats)
        for (const DrmFormat& f : *formats)
            list += format_name(f.format) + " ";
    alog(Log::Error, "%s: no 10-bit format for HDR; the plane offers: %s", output->name.c_str(), list.c_str());
}

} // namespace

// The metadata describes the screen (what Windows sends from the EDID, or
// from its calibration): its primaries and the luminances it can show.
backend::ImageDescription Output::hdr_description() const {
    backend::ImageDescription desc{};
    desc.primaries = NAMED_PRIMARIES_BT2020;
    desc.transfer_function = TRANSFER_FUNCTION_ST2084_PQ;
    if (screen->default_primaries)
        desc.mastering_display_primaries = *screen->default_primaries;
    else
        primaries_from_named(&desc.mastering_display_primaries, NAMED_PRIMARIES_BT2020);
    const double max = peak_nits() > 0 ? peak_nits() : 1000.0;
    desc.mastering_luminance.min = black_nits();
    desc.mastering_luminance.max = max;
    desc.max_cll = max;
    desc.max_fall = hdr_caps && hdr_caps->max_frame_avg_nits > 0 ? std::min(hdr_caps->max_frame_avg_nits, max) : max;
    return desc;
}

bool Output::apply_hdr() {
    const bool want = hdr && hdr_supported();
    bool ok = true;
    if (want != hdr_active()) {
        backend::OutputState state;
        state.allow_reconfiguration = true;
        if (want) {
            const backend::ImageDescription desc = hdr_description();
            state.set_image_description(&desc);
            // Night light moves from the gamma table into the renderer.
            if (screen->gamma_size() > 0)
                state.set_color_transform(nullptr);
            // At least 10 bits a channel, in whichever layout this screen's
            // plane and the renderer share (NVIDIA's may not be XRGB).
            ok = false;
            for (uint32_t format : {DRM_FORMAT_XRGB2101010, DRM_FORMAT_XBGR2101010, DRM_FORMAT_ARGB2101010,
                                    DRM_FORMAT_ABGR2101010, DRM_FORMAT_XBGR16161616F, DRM_FORMAT_ABGR16161616F}) {
                // Only the plane's own (testing any other logs an error).
                const FormatSet* plane = screen->primary_formats(BUFFER_CAP_DMABUF);
                if (plane && !plane->get(format))
                    continue;
                state.set_render_format(format);
                if (screen->test_state(state)) {
                    alog(Log::Info, "%s: HDR in %s", screen->name.c_str(), format_name(format).c_str());
                    ok = true;
                    break;
                }
            }
            if (!ok)
                log_formats(screen);
        } else {
            state.set_image_description(nullptr);
            state.set_render_format(DRM_FORMAT_XRGB8888);
            ok = screen->test_state(state);
        }
        ok = ok && screen->commit_state(state);
        if (!ok)
            alog(Log::Error, "%s: refused %s HDR", screen->name.c_str(), want ? "turning on" : "turning off");
    }
    scene_output->set_sdr_white_nits(hdr_active() ? float(sdr_white_nits()) : 0.0f);
    // Out of HDR the screen spreads sRGB over its whole gamut; in HDR atrium
    // does, as far as SDR color intensity says.
    if (hdr_active() && screen->default_primaries && sdr_color > 0) {
        ColorPrimaries srgb{}, spread{};
        primaries_from_named(&srgb, NAMED_PRIMARIES_SRGB);
        const float t = float(sdr_color) / 100.0f;
        const auto mix = [t](CieXY a, CieXY b) {
            return CieXY{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t};
        };
        spread.red = mix(srgb.red, screen->default_primaries->red);
        spread.green = mix(srgb.green, screen->default_primaries->green);
        spread.blue = mix(srgb.blue, screen->default_primaries->blue);
        spread.white = srgb.white;  // the same white: greys stay grey
        scene_output->set_sdr_primaries(&spread);
    } else {
        scene_output->set_sdr_primaries(nullptr);
    }
    screen->schedule_frame();
    return ok;
}

bool Output::apply_icc_hdr(std::string* error) {
    if (!scene_output)
        return false;
    const double was_max = calibrated_max_nits_, was_min = calibrated_min_nits_;
    calibrated_max_nits_ = calibrated_min_nits_ = 0;
    bool ok = true;
    if (icc_hdr.empty()) {
        scene_output->set_hdr_calibration(nullptr, nullptr);
    } else if (std::string why; auto cal = icc::load_hdr(icc_hdr, &why)) {
        std::unique_ptr<render::ColorLut> lut;
        if (cal->lut.size > 1) {
            lut = std::make_unique<render::ColorLut>();
            lut->size = cal->lut.size;
            lut->rgb = std::move(cal->lut.rgb);
        }
        scene_output->set_hdr_calibration(cal->matrix, std::move(lut));
        calibrated_max_nits_ = cal->max_nits;
        calibrated_min_nits_ = cal->min_nits;
        alog(Log::Info, "%s: HDR calibration %s (%.0f nits)", screen->name.c_str(), cal->description.c_str(),
             cal->max_nits);
    } else {
        alog(Log::Error, "%s: HDR calibration: %s", screen->name.c_str(), why.c_str());
        if (error)
            *error = why;
        scene_output->set_hdr_calibration(nullptr, nullptr);
        ok = false;
    }
    // New luminances: the screen is told, and SDR white follows.
    if (hdr_active() && (was_max != calibrated_max_nits_ || was_min != calibrated_min_nits_)) {
        resend_hdr_description_ = true;  // with the next frame
        scene_output->set_sdr_white_nits(float(sdr_white_nits()));
    }
    return ok;
}

bool Output::apply_icc(std::string* error) {
    if (!scene_output)
        return false;
    if (icc.empty()) {
        scene_output->set_color_lut(nullptr);
        return true;
    }
    std::string why;
    auto table = icc::load(icc, &why);
    if (!table) {
        alog(Log::Error, "%s: colour profile: %s", screen->name.c_str(), why.c_str());
        if (error)
            *error = why;
        scene_output->set_color_lut(nullptr);
        return false;
    }
    auto lut = std::make_unique<render::ColorLut>();
    lut->size = table->size;
    lut->rgb = std::move(table->rgb);
    scene_output->set_color_lut(std::move(lut));
    alog(Log::Info, "%s: colour profile %s", screen->name.c_str(), table->description.c_str());
    return true;
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
    if (render_timer_ && vblank_ns_ && period_ns_ && screen->backend.is_drm() &&
        screen->adaptive_sync_status != backend::AdaptiveSync::Enabled && !tearing_view(server, *this)) {
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
    // Now, not on the timer: one armed by an earlier frame must not fire
    // too and commit a second frame while this one's flip is pending.
    if (render_timer_fd_ >= 0) {
        const itimerspec off{};
        timerfd_settime(render_timer_fd_, 0, &off, nullptr);
    }
    aimed_ns_ = 0;
    render();
}

// Where the screen's vblanks fall, what the last frame took to composite,
// and whether it made the vblank it was aimed at: a miss widens the slack
// at once, a long run of hits narrows it again.
void Output::presented(int64_t when, int refresh) {
    if (refresh > 0)
        period_ns_ = refresh;
    else if (screen->refresh > 0)
        period_ns_ = 1000000000000LL / screen->refresh;
    vblank_ns_ = when;
    if (render_fence_ >= 0) {
        // (A fence stamped before the frame began isn't the GPU's: ignored.)
        if (const int64_t done = frame_timing::fence_signalled_ns(render_fence_))
            journal_.add(done - render_started_ns_);
        close(render_fence_);
        render_fence_ = -1;
    }
    if (aimed_ns_ && period_ns_) {
        if (when > aimed_ns_ + period_ns_ / 2) {
            slack_ns_ = std::min(slack_ns_ + kSlackStepNs * 4, period_ns_ / 2);
            on_time_ = 0;
        } else if (++on_time_ >= kOnTimeToNarrow) {
            slack_ns_ = std::max(slack_ns_ - kSlackStepNs, kMinSlackNs);
            on_time_ = 0;
        }
    }
    aimed_ns_ = 0;
    margin_ns_ = frame_timing::margin(journal_.estimate(), slack_ns_, kMinMarginNs, period_ns_);
}

void Output::render() {
    // A flip still pending: the frame event after it renders again.
    if (screen->frame_pending)
        return;
    const int64_t started = now_ns();
    server.animator.tick();
    // Night light, in linear light by the renderer, on SDR screens only
    // while no app sets the screen's gamma itself; screens without one
    // (nested) do without. A change repaints the whole screen.
    const NightLight* night = server.night_light.get();
    const bool own_gamma = server.wl->gamma->active(global.get());
    const night::Rgb w = night && (hdr_active() || !own_gamma) ? night->linear_white() : night::Rgb{1, 1, 1};
    scene_output->set_tint(float(w.r), float(w.g), float(w.b));
    // Variable refresh, as set: always, or while a fullscreen game is in front.
    const bool vrr = screen->adaptive_sync_supported &&
                     (adaptive_sync == "on" || (adaptive_sync == "games" && game_view(server, *this)));
    if (vrr_refused_ && *vrr_refused_ != vrr)
        vrr_refused_.reset();
    const bool switch_vrr = screen->adaptive_sync_supported &&
                            vrr != (screen->adaptive_sync_status == backend::AdaptiveSync::Enabled) &&
                            vrr_refused_ != vrr;
    // Frame callbacks go out even when nothing changed: a client that asked
    // for one without new damage (Qt between animation steps) would
    // otherwise wait forever, frozen mid-animation.
    if (!scene_output->needs_frame() && !switch_vrr) {
        send_frame_done();
        return;
    }
    server.capture_before_frame(this);
    backend::OutputState state;
    if (scene_output->build_state(&state)) {
        if (switch_vrr)
            state.set_adaptive_sync_enabled(vrr);
        const bool resend = std::exchange(resend_hdr_description_, false) && hdr_active();
        const backend::ImageDescription desc = resend ? hdr_description() : backend::ImageDescription{};
        if (resend)
            state.set_image_description(&desc);
        if (tearing_view(server, *this)) {
            // Show the frame the moment it is ready, torn if need be; fall
            // back to waiting for vblank when the hardware won't.
            state.tearing_page_flip = true;
            if (!screen->test_state(state))
                state.tearing_page_flip = false;
        }
        bool committed = screen->commit_state(state);
        if (!committed && resend) {
            // The screen won't take new metadata without a modeset: the
            // frame without it, and it waits for the next time HDR is set.
            alog(Log::Error, "%s: refused new HDR metadata", screen->name.c_str());
            state.committed &= ~backend::OutputState::ImageDescriptionField;
            committed = screen->commit_state(state);
        }
        // Timed on screens whose frames wait for a vblank (frame()).
        if (committed && screen->backend.is_drm()) {
            if (render_fence_ >= 0)
                close(render_fence_);
            render_fence_ = scene_output->render_fence();
            render_started_ns_ = started;
        }
        if (!committed && switch_vrr) {
            // The screen wouldn't take the switch: the frame without it, and
            // it's not tried again.
            alog(Log::Error, "%s: refused variable refresh", screen->name.c_str());
            state.committed &= ~backend::OutputState::AdaptiveSyncEnabled;
            screen->commit_state(state);
            vrr_refused_ = vrr;
        }
        if (committed && switch_vrr && server.ipc)
            server.ipc->broadcast("outputs", {{"event", "outputs.changed"}});
    }
    send_frame_done();
}

// The scene sends frame callbacks to what shows; panels covered by a
// fullscreen app get theirs here, or they could never draw themselves back
// over it (see LayerSurface::commit).
void Output::send_frame_done() {
    timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    scene_output->send_frame_done(&now);
    // Top and overlay only: a wallpaper animating under a window stays paused.
    const uint32_t ms = uint32_t(int64_t(now.tv_sec) * 1000 + now.tv_nsec / 1000000);
    for (auto layer : {ZWLR_LAYER_SHELL_V1_LAYER_TOP, ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY})
        for (LayerSurface* l : layers[layer])
            if (l->mapped)
                l->surface()->send_frame_done(ms);
}

namespace {

void arrange_layer(Output& o, std::vector<LayerSurface*>& list, Box& usable, bool exclusive) {
    const Box full = o.box;
    for (LayerSurface* l : list) {
        if (!l->ls->initialized())
            continue;
        if (exclusive != (l->ls->current().exclusive_zone > 0))
            continue;
        scene::layer_surface_v1_configure(l->scene_layer, &full, &usable);
        l->popups->set_position(l->tree->x, l->tree->y);
    }
}

} // namespace

void Output::arrange_layers() {
    if (!enabled())
        return;

    Box area = box;
    // Exclusive zones first, top to bottom, so panels shrink the usable area
    // before anything is placed inside it.
    for (int i = 3; i >= 0; --i)
        arrange_layer(*this, layers[i], area, true);

    if (!box_equal(&area, &usable)) {
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
    fullscreen_bg->set_enabled(std::ranges::any_of(server.views, [this](View* v) {
            return v->output == this && v->fullscreen_front() && v->visible();
        }));
}

} // namespace atrium
