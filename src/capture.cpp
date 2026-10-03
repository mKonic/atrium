// Screen and window capture (wlr-screencopy, ext-image-copy-capture,
// export-dmabuf, Hyprland's toplevel export): the protocol side is
// wl::Capture; this hands it frames. A screen's frame is what it last showed;
// a window's comes from its own capture scene (scene::CaptureSource).
#include "capture_state.hpp"
#include "cursor.hpp"
#include "render/pass.hpp"
#include "output.hpp"
#include "seat.hpp"
#include "server.hpp"
#include "view.hpp"

#include <drm_fourcc.h>
#include <sys/stat.h>

#include <functional>
#include <map>
#include <unordered_map>

namespace atrium {

namespace {

// Copies `box` of `src` (buffer pixels) into the whole of `dst`. `over`
// draws more on top (the pointer), which needs a render pass: a buffer in
// memory gets one through a GPU buffer made for it.
bool copy_into(render::Renderer* renderer, Buffer* src, const Box& box, Buffer* dst,
               const std::function<void(render::RenderPass*)>& over = {}, backend::Allocator* allocator = nullptr) {
    render::Texture* t = renderer->texture_from_buffer(src);
    if (!t)
        return false;
    auto draw = [&](Buffer* into) {
        render::RenderPass* pass = renderer->begin_buffer_pass(into, nullptr);
        if (!pass)
            return false;
        render::TextureOptions o{};
        o.texture = t;
        o.src_box = {double(box.x), double(box.y), double(box.width), double(box.height)};
        o.dst_box = {0, 0, into->width, into->height};
        o.filter_mode = render::SCALE_FILTER_NEAREST;
        o.blend_mode = render::BLEND_MODE_NONE;
        pass->add_texture(&o);
        if (over)
            over(pass);
        return pass->submit();
    };
    bool ok = false;
    void* data = nullptr;
    uint32_t format = 0;
    size_t stride = 0;
    if (buffer_begin_data_ptr_access(dst, BUFFER_DATA_PTR_ACCESS_WRITE, &data, &format, &stride)) {
        if (!over) {
            const render::ReadPixelsOptions o{
                .data = data, .format = format, .stride = uint32_t(stride), .dst_x = 0, .dst_y = 0, .src_box = box};
            ok = t->read_pixels(&o);
        } else if (allocator) {
            // One the GPU draws into: with the modifiers it reads (the
            // driver's pick; NVIDIA can't draw into an implicit one), else linear.
            std::vector<std::vector<uint64_t>> tries;
            if (const FormatSet* set = renderer->texture_formats(BUFFER_CAP_DMABUF))
                if (const DrmFormat* f = set->get(DRM_FORMAT_ARGB8888))
                    tries.push_back(f->modifiers);
            tries.push_back({DRM_FORMAT_MOD_LINEAR});
            for (const auto& mods : tries) {
                Buffer* tmp = allocator->allocate(dst->width, dst->height, DRM_FORMAT_ARGB8888, mods);
                if (!tmp)
                    continue;
                bool drawn_ok = draw(tmp);
                if (drawn_ok)
                    if (render::Texture* drawn = renderer->texture_from_buffer(tmp)) {
                        const render::ReadPixelsOptions o{.data = data, .format = format, .stride = uint32_t(stride)};
                        ok = drawn->read_pixels(&o);
                        drawn->destroy();
                    }
                buffer_drop(tmp);
                if (drawn_ok)
                    break;
            }
        }
        buffer_end_data_ptr_access(dst);
    } else {
        ok = draw(dst);
    }
    t->destroy();
    return ok;
}

dev_t render_device(render::Renderer* renderer) {
    struct stat st{};
    const int fd = renderer->drm_fd();
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
Box buffer_box(const Output& o, const std::optional<Box>& region) {
    int w = 0, h = 0;
    o.screen->transformed_resolution(&w, &h);
    if (!region)
        return {0, 0, o.screen->width, o.screen->height};
    const double s = o.screen->scale;
    Box b{int(std::lround((region->x - o.box.x) * s)), int(std::lround((region->y - o.box.y) * s)),
              int(std::lround(region->width * s)), int(std::lround(region->height * s))};
    box_transform(&b, &b, output_transform_invert(o.screen->transform), w, h);
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
            const Box b = buffer_box(*o, t.region);
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
        if (const FormatSet* set = renderer->texture_formats(BUFFER_CAP_DMABUF)) {
            if (const DrmFormat* f = set->get(format)) {
                c.dmabuf_format = format;
                c.dmabuf_modifiers = f->modifiers;
                c.dmabuf_device = render_device(renderer);
            }
        }
        return c;
    };

    connections_.push_back(cap.copy.connect([this](wl::Capture::Copy& copy) {
        if (Output* o = output_of(copy.target)) {
            auto& per = capture_->outputs[o];
            buffer_lock(copy.buffer);
            per.copies.push_back({copy, buffer_box(*o, copy.target.region)});
            if (!per.commit.connected())
                per.commit = o->screen->events.commit.connect(
                    [this, o](const backend::OutputState& st) { capture_output_frame(o, st); });
            // A copy that needn't wait for change: the screen is drawn anew.
            // One that waits gets the next frame something else brings
            // (scheduling one here would redraw an idle screen at its refresh
            // rate for as long as a stream is read).
            if (!copy.wait_for_damage) {
                if (o->scene_output)
                    o->scene_output->damage_whole();
                o->screen->schedule_frame();
            }
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
            per.source->on_frame = [this, v](Buffer* buffer, const pixman_region32_t*, const timespec& when) {
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
        buffer_lock(copy.buffer);
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

namespace {

template <class PerOutput>
bool wants_no_pointer(const PerOutput& per) {
    return std::ranges::any_of(per.copies, [](const auto& p) { return !p.copy.target.cursor; });
}

} // namespace

// The pointer drawn in software is in every frame: for a copy without it,
// the area it covers is drawn anew (what the buffer had there from an older
// frame may hold it)...
void Server::capture_before_frame(Output* o) {
    if (!capture_ || !cursor)
        return;
    auto it = capture_->outputs.find(o);
    if (it != capture_->outputs.end() && wants_no_pointer(it->second))
        cursor->damage_on(o->screen);
}

// ...and kept before the pointer goes over it.
void Server::capture_before_cursor(const backend::Output* screen, render::RenderPass* pass) {
    if (!capture_ || !cursor)
        return;
    for (auto& [o, per] : capture_->outputs) {
        if (o->screen != screen)
            continue;
        per.under_valid = false;
        Box b;
        if (!wants_no_pointer(per) || !cursor->drawn_box(screen, &b))
            return;
        // Drawn through a blend buffer (HDR, a profile), the target isn't the
        // frame: such a copy keeps the pointer.
        per.pointer_kept = pass->two_pass();
        if (per.pointer_kept)
            return;
        Box cut{};
        const Box all{0, 0, pass->width(), pass->height()};
        if (!box_intersection(&cut, &b, &all) || !per.under.ensure(*renderer, pass->width(), pass->height(), GL_RGBA8))
            return;
        pixman_region32_t r;
        pixman_region32_init_rect(&r, cut.x, cut.y, unsigned(cut.width), unsigned(cut.height));
        pass->copy(&r, per.under.get(), pass->target());
        pixman_region32_fini(&r);
        per.under_box = cut;
        per.under_valid = true;
        return;
    }
}

bool Server::capture_keeps_pointer(Output* o) const {
    if (!capture_)
        return false;
    auto it = capture_->outputs.find(o);
    return it != capture_->outputs.end() && it->second.pointer_kept;
}

// A screen showed a frame: what waited for one gets it.
void Server::capture_output_frame(Output* o, const backend::OutputState& st) {
    if (!(st.committed & backend::OutputState::Buffer) || !st.buffer)
        return;
    auto it = capture_->outputs.find(o);
    if (it == capture_->outputs.end())
        return;
    auto& per = it->second;
    Buffer* frame = st.buffer;
    timespec when;
    clock_gettime(CLOCK_MONOTONIC, &when);
    for (auto& p : std::exchange(per.copies, {})) {
        wl::Capture::Result r;
        // The pointer is in its own plane, not in the frame: drawn in here.
        std::function<void(render::RenderPass*)> over;
        if (p.copy.target.cursor && cursor && cursor->in_plane(o->screen))
            over = [this, o, &p](render::RenderPass* pass) {
                cursor->render_into_copy(o->screen, pass, p.box, p.copy.buffer->width, p.copy.buffer->height);
            };
        // Drawn in software but not wanted: what was under it, back over it.
        if (!p.copy.target.cursor && per.under_valid)
            over = [&per, &p](render::RenderPass* pass) {
                const double kx = double(p.copy.buffer->width) / p.box.width;
                const double ky = double(p.copy.buffer->height) / p.box.height;
                const Box& u = per.under_box;
                render::TextureDraw t;
                t.tex = per.under->texture();
                t.src = {double(u.x), double(u.y), double(u.width), double(u.height)};
                t.dst = {(u.x - p.box.x) * kx, (u.y - p.box.y) * ky, u.width * kx, u.height * ky};
                t.blend = false;
                t.filter = render::SCALE_FILTER_NEAREST;
                pass->add_texture(t);
            };
        r.ok = copy_into(renderer, frame, p.box, p.copy.buffer, over, allocator);
        r.when = when;
        r.transform = uint32_t(o->screen->transform);
        r.fail_reason = r.ok ? 0 : 1;
        buffer_unlock(p.copy.buffer);
        p.copy.done(r);
    }
    per.under_valid = false;
    for (auto& x : std::exchange(per.exports, {})) {
        DmabufAttributes attrs{};
        if (buffer_get_dmabuf(frame, &attrs))
            x.done(&attrs, when);
        else
            x.done(nullptr, when);
    }
    per.commit.disconnect();
}

// A window's capture scene drew a frame.
void Server::capture_view_frame(View* v, Buffer* frame, const timespec& when) {
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
        buffer_unlock(c.buffer);
        c.done(r);
    }
    if (resized && v->handle_)
        wl->capture->constraints_changed({.toplevel = v->handle_});
}

void Server::capture_view_gone(View* v) {
    if (!capture_)
        return;
    // Sessions on it stop even if nothing was copied yet.
    if (v->handle_)
        wl->capture->stop({.toplevel = v->handle_});
    auto it = capture_->views.find(v);
    if (it == capture_->views.end())
        return;
    auto& per = it->second;
    for (auto& c : per.copies)
        c.done({.ok = false, .fail_reason = 2});
    // Buffers and the timer go with it; the source with the capture scene.
    capture_->views.erase(it);
}

void Server::capture_output_gone(Output* o) {
    if (!capture_)
        return;
    if (o->global)
        wl->capture->stop({.output = o->global.get()});
    auto it = capture_->outputs.find(o);
    if (it == capture_->outputs.end())
        return;
    for (auto& p : it->second.copies)
        p.copy.done({.ok = false, .fail_reason = 2});
    for (auto& x : it->second.exports)
        x.done(nullptr, {});
    capture_->outputs.erase(it);
}

} // namespace atrium
