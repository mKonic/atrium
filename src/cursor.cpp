#include "render/pass.hpp"
#include "cursor.hpp"

#include "backend/allocator.hpp"

#include "render/matrix.hpp"
#include "render/renderer.hpp"
#include "wlr.hpp"

extern "C" {
#include <wlr/render/pass.h>
#include <wlr/xcursor.h>
}

#include <drm_fourcc.h>

#include <algorithm>
#include <cmath>

namespace atrium {

const char* resize_cursor_name(uint32_t edges) {
    if (edges & EDGE_TOP)
        return edges & EDGE_RIGHT ? "ne-resize" : edges & EDGE_LEFT ? "nw-resize" : "n-resize";
    if (edges & EDGE_BOTTOM)
        return edges & EDGE_RIGHT ? "se-resize" : edges & EDGE_LEFT ? "sw-resize" : "s-resize";
    if (edges & EDGE_RIGHT)
        return "e-resize";
    if (edges & EDGE_LEFT)
        return "w-resize";
    return "se-resize";
}

struct Cursor::Screen {
    backend::Output* output = nullptr;
    wl::Connection commit;
    render::Texture* texture = nullptr;
    int width = 0, height = 0;  // in the screen's pixels
    int hot_x = 0, hot_y = 0;
    bool plane = false;
    std::unique_ptr<backend::Swapchain> swapchain;
    Buffer* front = nullptr;  // in the plane now
    Box drawn{};  // drawn in software here last (transformed pixels)

    ~Screen() {
        if (texture)
            texture->destroy();
        if (front)
            buffer_unlock(front);
    }
};

namespace {

// sRGB into an HDR screen's signal, for the plane (the scene's frames do
// their own conversion): its primaries, SDR white at the reference
// luminance, then its transfer function. As wlroots does for its cursors.
render::OutputColor color_for(const std::optional<backend::ImageDescription>& d) {
    render::OutputColor c;
    if (!d)
        return c;
    wlr_color_primaries srgb, target;
    wlr_color_primaries_from_named(&srgb, WLR_COLOR_NAMED_PRIMARIES_SRGB);
    wlr_color_primaries_from_named(&target, d->primaries);
    wlr_color_primaries_transform_absolute_colorimetric(&srgb, &target, c.matrix);
    const wlr_color_luminances lum = render::default_luminance(d->transfer_function);
    for (float& m : c.matrix)
        m *= float(lum.reference / lum.max);
    c.tf = render::output_tf(d->transfer_function);
    return c;
}

// ARGB8888 with the modifiers both the plane and the renderer take.
bool cursor_format(const backend::Output& o, std::vector<uint64_t>* mods) {
    const wlr_drm_format_set* render = o.renderer->egl().render_formats();
    const wlr_drm_format* rf = render ? wlr_drm_format_set_get(render, DRM_FORMAT_ARGB8888) : nullptr;
    if (!rf)
        return false;
    const wlr_drm_format_set* plane = o.cursor_formats(BUFFER_CAP_DMABUF);
    const wlr_drm_format* pf = plane ? wlr_drm_format_set_get(plane, DRM_FORMAT_ARGB8888) : nullptr;
    if (plane && !pf)
        return false;
    mods->clear();
    for (size_t i = 0; i < rf->len; ++i)
        if (!pf || std::find(pf->modifiers, pf->modifiers + pf->len, rf->modifiers[i]) != pf->modifiers + pf->len)
            mods->push_back(rf->modifiers[i]);
    return !mods->empty();
}

} // namespace

Cursor::Cursor(OutputLayout& layout, wl_event_loop* loop) : layout_(layout), loop_(loop) {
    layout_change_ = layout_.change.connect([this] {
        sync_screens();
        // Screens moved: the pointer stays on one.
        if (!layout_.empty())
            warp_closest(x, y);
    });
    sync_screens();
}

Cursor::~Cursor() {
    layout_change_.disconnect();
    if (animation_)
        wl_event_source_remove(animation_);
    for (auto& s : screens_)
        if (s->plane)
            s->output->set_cursor(nullptr, 0, 0);
    screens_.clear();
    if (buffer_)
        buffer_unlock(buffer_);
}

Cursor::Screen* Cursor::screen_of(const backend::Output* o) const {
    auto it = std::ranges::find_if(screens_, [o](const auto& s) { return s->output == o; });
    return it == screens_.end() ? nullptr : it->get();
}

void Cursor::sync_screens() {
    // Gone from the layout: off their plane, and forgotten.
    std::erase_if(screens_, [this](const auto& s) {
        if (layout_.contains(s->output))
            return false;
        if (s->plane)
            s->output->set_cursor(nullptr, 0, 0);
        return true;
    });
    for (backend::Output* o : layout_.outputs()) {
        if (screen_of(o))
            continue;
        auto s = std::make_unique<Screen>();
        s->output = o;
        Screen* raw = s.get();
        s->commit = o->events.commit.connect([this, raw](const backend::OutputState& st) {
            using F = backend::OutputState;
            if (st.committed & (F::Scale | F::Transform | F::ModeField | F::Enabled | F::ImageDescriptionField))
                refresh(*raw);
        });
        screens_.push_back(std::move(s));
        refresh(*raw);
    }
    for (auto& s : screens_)
        place(*s);
}

void Cursor::refresh_all() {
    for (auto& s : screens_)
        refresh(*s);
}

void Cursor::refresh(Screen& s) {
    damage(s);
    s.drawn = {};
    if (s.texture)
        s.texture->destroy();
    s.texture = nullptr;
    s.width = s.height = s.hot_x = s.hot_y = 0;
    backend::Output& o = *s.output;
    if (o.renderer && o.enabled) {
        if (kind_ == Kind::XCursor) {
            // The theme at this screen's scale: one image pixel per screen pixel.
            wlr_xcursor_manager_load(manager_, o.scale);
            if (wlr_xcursor* xc = wlr_xcursor_manager_get_xcursor(manager_, name_.c_str(), o.scale)) {
                const wlr_xcursor_image* img = xc->images[frame_ % xc->image_count];
                s.texture = o.renderer->texture_from_pixels(DRM_FORMAT_ARGB8888, img->width * 4, img->width,
                                                    img->height, img->buffer);
                s.width = int(img->width);
                s.height = int(img->height);
                s.hot_x = int(img->hotspot_x);
                s.hot_y = int(img->hotspot_y);
            }
        } else if (kind_ == Kind::Buffer) {
            s.texture = o.renderer->texture_from_buffer(buffer_);
            if (s.texture) {
                const float k = o.scale / buffer_scale_;
                s.width = int(std::lround(s.texture->width * k));
                s.height = int(std::lround(s.texture->height * k));
                s.hot_x = int(std::lround(hot_x_ * k));
                s.hot_y = int(std::lround(hot_y_ * k));
            }
        }
    }
    const bool was_plane = s.plane;
    s.plane = try_plane(s);
    if (was_plane && !s.plane)
        o.set_cursor(nullptr, 0, 0);
    place(s);
}

bool Cursor::try_plane(Screen& s) {
    backend::Output& o = *s.output;
    if (!o.has_cursor_plane() || !o.allocator || !o.renderer || !o.enabled)
        return false;
    if (!s.texture) {
        // Nothing to show: an empty plane.
        if (!o.set_cursor(nullptr, 0, 0))
            return false;
        if (s.front)
            buffer_unlock(s.front);
        s.front = nullptr;
        o.update_needs_frame();
        return true;
    }
    int w = s.width, h = s.height;
    const auto sizes = o.cursor_sizes();
    if (!sizes.empty()) {
        auto fit = std::ranges::find_if(sizes, [&](auto sz) { return s.width <= sz.first && s.height <= sz.second; });
        if (fit == sizes.end())
            return false;  // too big for the plane
        w = fit->first;
        h = fit->second;
    }
    if (!s.swapchain || s.swapchain->width != w || s.swapchain->height != h) {
        std::vector<uint64_t> mods;
        if (!cursor_format(o, &mods))
            return false;
        s.swapchain = std::make_unique<backend::Swapchain>(*o.allocator, w, h, DRM_FORMAT_ARGB8888, std::move(mods));
    }
    Buffer* buf = s.swapchain->acquire();
    if (!buf)
        return false;

    Box dst{0, 0, s.width, s.height};
    box_transform(&dst, &dst, output_transform_invert(o.transform), buf->width, buf->height);
    render::BufferPassOptions opts{};
    opts.color = color_for(o.image_description);
    render::RenderPass* pass = o.renderer->begin_buffer_pass(buf, &opts);
    if (!pass) {
        buffer_unlock(buf);
        return false;
    }
    render::RectOptions clear{};
    clear.box = {0, 0, buf->width, buf->height};
    clear.blend_mode = render::BLEND_MODE_NONE;
    pass->add_rect(&clear);
    render::TextureOptions tex{};
    tex.texture = s.texture;
    tex.src_box = {0, 0, double(s.texture->width), double(s.texture->height)};
    tex.dst_box = dst;
    tex.transform = o.transform;
    tex.filter_mode = render::SCALE_FILTER_BILINEAR;
    pass->add_texture(&tex);
    if (!pass->submit()) {
        buffer_unlock(buf);
        return false;
    }

    Box hot{s.hot_x, s.hot_y, 0, 0};
    box_transform(&hot, &hot, output_transform_invert(o.transform), buf->width, buf->height);
    const bool ok = o.set_cursor(buf, hot.x, hot.y);
    if (ok) {
        // Held while the plane shows it: the swapchain won't hand it out.
        if (s.front)
            buffer_unlock(s.front);
        s.front = buf;
        o.update_needs_frame();
    } else {
        buffer_unlock(buf);
    }
    return ok;
}

Box Cursor::box_on(const Screen& s) const {
    const Box ob = layout_.box(s.output);
    const float k = s.output->scale;
    return {int((x - ob.x) * k) - s.hot_x, int((y - ob.y) * k) - s.hot_y, s.width, s.height};
}

void Cursor::damage(Screen& s) {
    if (s.plane || s.drawn.width <= 0 || s.drawn.height <= 0)
        return;
    pixman_region32_t r;
    pixman_region32_init_rect(&r, s.drawn.x, s.drawn.y, unsigned(s.drawn.width), unsigned(s.drawn.height));
    s.output->events.damage.emit(&r);
    pixman_region32_fini(&r);
}

void Cursor::place(Screen& s) {
    if (s.plane) {
        const Box ob = layout_.box(s.output);
        const float k = s.output->scale;
        if (s.output->move_cursor(int((x - ob.x) * k), int((y - ob.y) * k)))
            s.output->update_needs_frame();
        return;
    }
    damage(s);  // where it was
    s.drawn = s.texture ? box_on(s) : Box{};
    damage(s);  // where it is
}

void Cursor::warp_closest(double lx, double ly) {
    double cx, cy;
    layout_.closest_point(nullptr, lx, ly, &cx, &cy);
    if (std::isnan(cx) || std::isnan(cy))
        return;
    x = cx;
    y = cy;
    for (auto& s : screens_)
        place(*s);
}

bool Cursor::warp(double lx, double ly) {
    if (!layout_.output_at(lx, ly))
        return false;
    x = lx;
    y = ly;
    for (auto& s : screens_)
        place(*s);
    return true;
}

void Cursor::move(double dx, double dy) {
    warp_closest(x + dx, y + dy);
}

void Cursor::absolute_to_layout(double fx, double fy, double* lx, double* ly) const {
    const Box e = layout_.extents();
    *lx = e.x + fx * e.width;
    *ly = e.y + fy * e.height;
}

void Cursor::set_xcursor(wlr_xcursor_manager* manager, const char* name) {
    if (kind_ == Kind::XCursor && manager_ == manager && name_ == name)
        return;
    if (buffer_)
        buffer_unlock(buffer_);
    buffer_ = nullptr;
    kind_ = Kind::XCursor;
    manager_ = manager;
    name_ = name;
    frame_ = 0;
    refresh_all();
    schedule_animation();
}

void Cursor::set_buffer(Buffer* buffer, int hotspot_x, int hotspot_y, float scale) {
    Buffer* locked = buffer ? buffer_lock(buffer) : nullptr;
    if (buffer_)
        buffer_unlock(buffer_);
    buffer_ = locked;
    kind_ = buffer ? Kind::Buffer : Kind::None;
    hot_x_ = hotspot_x;
    hot_y_ = hotspot_y;
    buffer_scale_ = scale > 0 ? scale : 1;
    refresh_all();
    schedule_animation();
}

void Cursor::unset_image() {
    set_buffer(nullptr, 0, 0, 1);
}

void Cursor::schedule_animation() {
    if (animation_) {
        wl_event_source_remove(animation_);
        animation_ = nullptr;
    }
    if (kind_ != Kind::XCursor)
        return;
    // The theme's frames, at the delay the theme gives (scale 1 is as good
    // as any: every scale has the same frames).
    wlr_xcursor_manager_load(manager_, 1);
    wlr_xcursor* xc = wlr_xcursor_manager_get_xcursor(manager_, name_.c_str(), 1);
    if (!xc || xc->image_count < 2)
        return;
    const uint32_t delay = xc->images[frame_ % xc->image_count]->delay;
    if (!delay)
        return;
    animation_ = wl_event_loop_add_timer(
        loop_,
        [](void* data) {
            auto* c = static_cast<Cursor*>(data);
            ++c->frame_;
            c->refresh_all();
            c->schedule_animation();
            return 0;
        },
        this);
    wl_event_source_timer_update(animation_, int(delay));
}

void Cursor::reset_render() {
    for (auto& s : screens_) {
        if (s->front)
            buffer_unlock(s->front);
        s->front = nullptr;
        s->swapchain.reset();
        refresh(*s);
    }
}

bool Cursor::in_plane(const backend::Output* o) const {
    const Screen* s = screen_of(o);
    return s && s->plane;
}

void Cursor::render(const backend::Output* o, render::RenderPass* pass, const pixman_region32_t* damage) {
    Screen* s = screen_of(o);
    if (!s || s->plane || !s->texture)
        return;
    int w, h;
    o->transformed_resolution(&w, &h);
    Box box = box_on(*s);
    box_transform(&box, &box, output_transform_invert(o->transform), w, h);
    pixman_region32_t clip;
    pixman_region32_init_rect(&clip, box.x, box.y, unsigned(box.width), unsigned(box.height));
    if (damage)
        pixman_region32_intersect(&clip, &clip, damage);
    if (pixman_region32_not_empty(&clip)) {
        render::TextureOptions tex{};
        tex.texture = s->texture;
        tex.src_box = {0, 0, double(s->texture->width), double(s->texture->height)};
        tex.dst_box = box;
        tex.clip = &clip;
        tex.transform = o->transform;
        tex.filter_mode = render::SCALE_FILTER_BILINEAR;
        pass->add_texture(&tex);
    }
    pixman_region32_fini(&clip);
}

} // namespace atrium
