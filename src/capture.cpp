// Screen and window capture (wlr-screencopy, ext-image-copy-capture,
// export-dmabuf, Hyprland's toplevel export): the protocol side is
// wl::Capture; this hands it frames. A screen's frame is what it last showed;
// a window's comes from its own capture scene (scene::CaptureSource).
#include "capture_state.hpp"
#include "output.hpp"
#include "seat.hpp"
#include "server.hpp"
#include "view.hpp"

#include <drm_fourcc.h>
#include <sys/stat.h>

#include <map>
#include <unordered_map>

namespace atrium {

namespace {

// Copies `box` of `src` (buffer pixels) into the whole of `dst`.
bool copy_into(wlr_renderer* renderer, wlr_buffer* src, const wlr_box& box, wlr_buffer* dst) {
    wlr_texture* t = wlr_texture_from_buffer(renderer, src);
    if (!t)
        return false;
    bool ok = false;
    void* data = nullptr;
    uint32_t format = 0;
    size_t stride = 0;
    if (wlr_buffer_begin_data_ptr_access(dst, WLR_BUFFER_DATA_PTR_ACCESS_WRITE, &data, &format, &stride)) {
        const wlr_texture_read_pixels_options o{
            .data = data, .format = format, .stride = uint32_t(stride), .dst_x = 0, .dst_y = 0, .src_box = box};
        ok = wlr_texture_read_pixels(t, &o);
        wlr_buffer_end_data_ptr_access(dst);
    } else if (wlr_render_pass* pass = wlr_renderer_begin_buffer_pass(renderer, dst, nullptr)) {
        wlr_render_texture_options o{};
        o.texture = t;
        o.src_box = {double(box.x), double(box.y), double(box.width), double(box.height)};
        o.dst_box = {0, 0, dst->width, dst->height};
        o.filter_mode = WLR_SCALE_FILTER_NEAREST;
        o.blend_mode = WLR_RENDER_BLEND_MODE_NONE;
        wlr_render_pass_add_texture(pass, &o);
        ok = wlr_render_pass_submit(pass);
    }
    wlr_texture_destroy(t);
    return ok;
}

dev_t render_device(wlr_renderer* renderer) {
    struct stat st{};
    const int fd = wlr_renderer_get_drm_fd(renderer);
    return fd >= 0 && fstat(fd, &st) == 0 ? st.st_rdev : 0;
}

} // namespace

namespace {

Output* output_of(const wl::Capture::Target& t) {
    return t.output ? static_cast<Output*>(t.output->data) : nullptr;
}

View* view_of(const wl::Capture::Target& t) {
    return t.toplevel ? static_cast<View*>(t.toplevel->data) : nullptr;
}

// A part of the screen (layout coordinates) in its buffer's pixels.
wlr_box buffer_box(const Output& o, const std::optional<wl::Box>& region) {
    int w = 0, h = 0;
    o.screen->transformed_resolution(&w, &h);
    if (!region)
        return {0, 0, o.screen->width, o.screen->height};
    const double s = o.screen->scale;
    wlr_box b{int(std::lround((region->x - o.box.x) * s)), int(std::lround((region->y - o.box.y) * s)),
              int(std::lround(region->width * s)), int(std::lround(region->height * s))};
    wlr_box_transform(&b, &b, wlr_output_transform_invert(o.screen->transform), w, h);
    return b;
}

} // namespace

void Server::setup_capture() {
    capture_ = std::make_unique<CaptureState>();
    wl::Capture& cap = *wl->capture;

    cap.constraints = [this](const wl::Capture::Target& t) -> std::optional<wl::Capture::Constraints> {
        int w = 0, h = 0;
        uint32_t format = DRM_FORMAT_XRGB8888;
        if (Output* o = output_of(t)) {
            if (!o->enabled())
                return std::nullopt;
            const wlr_box b = buffer_box(*o, t.region);
            w = b.width;
            h = b.height;
            if (o->screen->render_format)
                format = o->screen->render_format;
        } else if (View* v = view_of(t)) {
            auto it = capture_->views.find(v);
            if (it != capture_->views.end() && it->second.source && it->second.source->width() > 0) {
                w = it->second.source->width();
                h = it->second.source->height();
            } else {
                w = v->geom.width;
                h = v->geom.height - v->top();
            }
            format = DRM_FORMAT_ARGB8888;
        } else {
            return std::nullopt;
        }
        if (w <= 0 || h <= 0)
            return std::nullopt;
        wl::Capture::Constraints c;
        c.width = w;
        c.height = h;
        c.shm_format = format;
        c.shm_stride = uint32_t(w) * 4;
        if (const wlr_drm_format_set* set = wlr_renderer_get_texture_formats(renderer, WLR_BUFFER_CAP_DMABUF)) {
            if (const wlr_drm_format* f = wlr_drm_format_set_get(set, format)) {
                c.dmabuf_format = format;
                c.dmabuf_modifiers.assign(f->modifiers, f->modifiers + f->len);
                c.dmabuf_device = render_device(renderer);
            }
        }
        return c;
    };

    connections_.push_back(cap.copy.connect([this](wl::Capture::Copy& copy) {
        if (Output* o = output_of(copy.target)) {
            auto& per = capture_->outputs[o];
            wlr_buffer_lock(copy.buffer);
            per.copies.push_back({copy, buffer_box(*o, copy.target.region)});
            if (!per.commit.connected())
                per.commit = o->screen->events.commit.connect(
                    [this, o](const backend::OutputState& st) { capture_output_frame(o, st); });
            // A copy that needn't wait for change: the screen is drawn anew.
            if (!copy.wait_for_damage && o->scene_output)
                o->scene_output->damage_whole();
            o->screen->schedule_frame();
            return;
        }
        View* v = view_of(copy.target);
        if (!v || !v->capture_scene_) {
            copy.done({.ok = false, .fail_reason = 2});
            return;
        }
        auto& per = capture_->views[v];
        if (!per.source) {
            per.source = scene::CaptureSource::create(v->capture_scene_, loop, allocator, renderer);
            per.source->on_frame = [this, v](wlr_buffer* buffer, const pixman_region32_t*, const timespec& when) {
                capture_view_frame(v, buffer, when);
            };
            per.idle = wl_event_loop_add_timer(loop, [](void* data) {
                auto* p = static_cast<CaptureState::PerView*>(data);
                if (p->copies.empty() && p->source && p->running) {
                    p->source->stop();
                    p->running = false;
                }
                return 0;
            }, &per);
        }
        wlr_buffer_lock(copy.buffer);
        per.copies.push_back(copy);
        // Nobody asked for a while: the window's own scene stops drawing.
        wl_event_source_timer_update(per.idle, 3000);
        if (!per.running) {
            per.running = true;
            per.source->start();  // draws a first frame straight away
        } else {
            per.source->request_frame(!copy.wait_for_damage);
        }
    }));

    connections_.push_back(cap.export_frame.connect([this](wl::Capture::Export& e) {
        Output* o = e.output ? static_cast<Output*>(e.output->data) : nullptr;
        if (!o) {
            e.done(nullptr, {});
            return;
        }
        auto& per = capture_->outputs[o];
        per.exports.push_back(e);
        if (!per.commit.connected())
            per.commit = o->screen->events.commit.connect(
                [this, o](const backend::OutputState& st) { capture_output_frame(o, st); });
        o->screen->schedule_frame();
    }));
}

// A screen showed a frame: what waited for one gets it.
void Server::capture_output_frame(Output* o, const backend::OutputState& st) {
    if (!(st.committed & backend::OutputState::Buffer) || !st.buffer)
        return;
    auto it = capture_->outputs.find(o);
    if (it == capture_->outputs.end())
        return;
    auto& per = it->second;
    wlr_buffer* frame = st.buffer;
    timespec when;
    clock_gettime(CLOCK_MONOTONIC, &when);
    for (auto& p : std::exchange(per.copies, {})) {
        wl::Capture::Result r;
        r.ok = copy_into(renderer, frame, p.box, p.copy.buffer);
        r.when = when;
        r.transform = uint32_t(o->screen->transform);
        r.fail_reason = r.ok ? 0 : 1;
        wlr_buffer_unlock(p.copy.buffer);
        p.copy.done(r);
    }
    for (auto& x : std::exchange(per.exports, {})) {
        wlr_dmabuf_attributes attrs{};
        if (wlr_buffer_get_dmabuf(frame, &attrs))
            x.done(&attrs, when);
        else
            x.done(nullptr, when);
    }
    per.commit.disconnect();
}

// A window's capture scene drew a frame.
void Server::capture_view_frame(View* v, wlr_buffer* frame, const timespec& when) {
    auto it = capture_->views.find(v);
    if (it == capture_->views.end())
        return;
    auto& per = it->second;
    std::vector<wl::Capture::Copy> copies = std::exchange(per.copies, {});
    bool resized = false;
    for (auto& c : copies) {
        wl::Capture::Result r;
        r.when = when;
        if (c.buffer->width == frame->width && c.buffer->height == frame->height) {
            r.ok = copy_into(renderer, frame, {0, 0, frame->width, frame->height}, c.buffer);
            r.fail_reason = r.ok ? 0 : 1;
        } else {
            r.fail_reason = 1;  // the window changed size: new constraints
            resized = true;
        }
        wlr_buffer_unlock(c.buffer);
        c.done(r);
    }
    if (resized && v->handle_)
        wl->capture->constraints_changed({.toplevel = v->handle_});
}

void Server::capture_view_gone(View* v) {
    if (!capture_)
        return;
    auto it = capture_->views.find(v);
    if (it == capture_->views.end())
        return;
    auto& per = it->second;
    for (auto& c : per.copies)
        c.done({.ok = false, .fail_reason = 2});
    // Buffers and the timer go with it; the source with the capture scene.
    capture_->views.erase(it);
    if (v->handle_)
        wl->capture->stop({.toplevel = v->handle_});
}

void Server::capture_output_gone(Output* o) {
    if (!capture_)
        return;
    auto it = capture_->outputs.find(o);
    if (it == capture_->outputs.end())
        return;
    for (auto& p : it->second.copies)
        p.copy.done({.ok = false, .fail_reason = 2});
    for (auto& x : it->second.exports)
        x.done(nullptr, {});
    capture_->outputs.erase(it);
    if (o->global)
        wl->capture->stop({.output = o->global.get()});
}

} // namespace atrium
