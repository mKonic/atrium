// Drawing the scene onto an output (wlr_scene_output_build_state and what
// it needs, from wlroots/scenefx).

#include "scene/internal.hpp"
#include "backend/allocator.hpp"
#include "backend/backend.hpp"

#include "wl/compositor.hpp"
#include "wl/desktop.hpp"
#include "wl/dmabuf.hpp"
#include "wl/timing.hpp"
#include "warp.hpp"

#include "render/matrix.hpp"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <vector>

namespace atrium::scene {

namespace {

constexpr int kDmabufFeedbackDebounce = 30;
constexpr int kHighlightFadeMs = 250;

int64_t ns_of(const timespec& t) { return int64_t(t.tv_sec) * 1000000000 + t.tv_nsec; }

void logical_to_buffer(pixman_region32_t* region, const RenderData& d, bool round_up) {
    scale_region(region, d.scale, round_up);
    wlr_region_transform(region, region, wlr_output_transform_invert(d.transform), d.trans_width, d.trans_height);
}

// A layout box in the output's buffer pixels: whole pixels exactly as
// wlroots rounds them when it can be, fractional otherwise.
render::FBox to_buffer(const render::FBox& b, const RenderData& d) {
    const bool whole = b.x == std::floor(b.x) && b.y == std::floor(b.y) && b.width == std::floor(b.width) &&
                       b.height == std::floor(b.height);
    if (whole) {
        wlr_box box{int(b.x) - d.logical.x, int(b.y) - d.logical.y, int(b.width), int(b.height)};
        scale_box(&box, d.scale);
        wlr_box_transform(&box, &box, wlr_output_transform_invert(d.transform), d.trans_width, d.trans_height);
        return render::FBox::of(box);
    }
    wlr_fbox f{(b.x - d.logical.x) * d.scale, (b.y - d.logical.y) * d.scale, b.width * d.scale, b.height * d.scale};
    wlr_fbox_transform(&f, &f, wlr_output_transform_invert(d.transform), d.trans_width, d.trans_height);
    return {f.x, f.y, f.width, f.height};
}

// Radii through an output transform, then scaled.
render::Corners corners_of(Radii r, wl_output_transform t, double scale) {
    if (t & WL_OUTPUT_TRANSFORM_FLIPPED)
        r = {r.tr, r.tl, r.bl, r.br};
    const unsigned turns = t & 3;
    if (turns) {
        const int p[4] = {r.tl, r.tr, r.br, r.bl};
        r = {p[turns % 4], p[(turns + 1) % 4], p[(turns + 2) % 4], p[(turns + 3) % 4]};
    }
    const float s = float(scale);
    return {r.tl * s, r.tr * s, r.br * s, r.bl * s};
}

render::CutOut cut_of(const CutOut& c, const Walk& w, const RenderData& d, wl_output_transform t) {
    if (c.empty())
        return {};
    const render::FBox f = to_buffer({w.x + c.area.x * w.scale, w.y + c.area.y * w.scale, c.area.width * w.scale,
                                      c.area.height * w.scale},
                                     d);
    render::CutOut out;
    out.area = {int(std::lround(f.x)), int(std::lround(f.y)), int(std::lround(f.width)), int(std::lround(f.height))};
    out.corners = corners_of(c.corners, t, w.scale * d.scale);
    return out;
}

float luminance_multiplier(const wlr_color_luminances& src, const wlr_color_luminances& dst) {
    return (dst.reference / src.reference) * (src.max / dst.max);
}

const backend::ImageDescription* pending_image_description(backend::Output* o, const backend::OutputState* s) {
    const auto& d = (s->committed & backend::OutputState::ImageDescriptionField) ? s->image_description
                                                                                  : o->image_description;
    return d ? &*d : nullptr;
}

void pending_resolution(backend::Output* o, const backend::OutputState* s, int* w, int* h) {
    if (s->committed & backend::OutputState::ModeField) {
        if (s->mode_type == backend::OutputState::ModeType::Fixed) {
            *w = s->mode->width;
            *h = s->mode->height;
        } else {
            *w = s->custom_mode.width;
            *h = s->custom_mode.height;
        }
        return;
    }
    *w = o->width;
    *h = o->height;
}

int tf_index(wlr_color_transfer_function tf) {
    switch (tf) {
    case WLR_COLOR_TRANSFER_FUNCTION_ST2084_PQ:
        return 1;
    case WLR_COLOR_TRANSFER_FUNCTION_EXT_LINEAR:
        return 2;
    case WLR_COLOR_TRANSFER_FUNCTION_SRGB:
        return 3;
    default:
        return 0;
    }
}

// Tells a surface which buffers suit where it is shown: `scanout` when it
// could go straight to that screen, else rendered.
void send_dmabuf_feedback(Scene* scene, Buffer* b, backend::Output* scanout) {
    const Protocols& p = scene->protocols;
    if (!p.dmabuf || !p.dmabuf_feedback || !b->surface())
        return;
    const std::pair<backend::Output*, bool> key{scanout, true};
    if (b->feedback_sent == key)
        return;
    b->feedback_sent = key;
    b->surface()->surface->set_preferred_transform(scanout ? uint32_t(scanout->transform) : uint32_t(WL_OUTPUT_TRANSFORM_NORMAL));
    p.dmabuf->set_surface_feedback(b->surface()->surface, p.dmabuf_feedback(scanout));
}

bool scanout_colour_allowed(const backend::ImageDescription* desc, const Buffer* b) {
    if (b->transfer_function == 0 && b->primaries == 0)
        return desc == nullptr;
    if (desc)
        return desc->transfer_function == b->transfer_function && desc->primaries == b->primaries;
    return b->transfer_function == WLR_COLOR_TRANSFER_FUNCTION_GAMMA22 &&
           b->primaries == WLR_COLOR_NAMED_PRIMARIES_SRGB;
}

} // namespace

// ---- lifetime -----------------------------------------------------------

SceneOutput::SceneOutput(Scene* s, backend::Output* o) : output(o), scene(s) {
    wlr_damage_ring_init(&damage_ring);
    pixman_region32_init(&pending_commit_damage);
    wl_signal_init(&events.destroy);
    wl_list_init(&link);
}

SceneOutput::~SceneOutput() = default;

SceneOutput* SceneOutput::create(Scene* scene, backend::Output* output) {
    auto* so = new SceneOutput(scene, output);

    // The lowest free index.
    int prev_index = -1;
    wl_list* prev_link = &scene->outputs;
    SceneOutput* cur;
    wl_list_for_each(cur, &scene->outputs, link) {
        if (prev_index + 1 != cur->index)
            break;
        prev_index = cur->index;
        prev_link = &cur->link;
    }
    const int drm_fd = output->backend.drm_fd();
    if (drm_fd >= 0 && output->backend.supports_timelines() && output->renderer &&
        output->renderer->features.timeline) {
        so->in_timeline_ = wlr_drm_syncobj_timeline_create(drm_fd);
        so->out_timeline_ = wlr_drm_syncobj_timeline_create(drm_fd);
    }
    so->index = uint8_t(prev_index + 1);
    assert(so->index < 64);
    wl_list_remove(&so->link);
    wl_list_insert(prev_link, &so->link);

    // Gone with the screen.
    so->output_destroy_ = output->events.destroy.connect([so] { so->destroy(); });
    so->commit_ = output->events.commit.connect([so](const backend::OutputState& state) {
        const backend::OutputState* st = &state;
        // Damage the backend took is done with.
        if (st->committed & backend::OutputState::Buffer) {
            if (st->committed & backend::OutputState::Damage)
                pixman_region32_subtract(&so->pending_commit_damage, &so->pending_commit_damage, &st->damage);
            else
                pixman_region32_clear(&so->pending_commit_damage);
        }
        const bool force = st->committed & (backend::OutputState::Transform | backend::OutputState::Scale |
                                            backend::OutputState::Subpixel);
        if (force || (st->committed & (backend::OutputState::ModeField | backend::OutputState::Enabled)))
            so->update_geometry(force);
        if (so->scene->debug_damage == Scene::DebugDamage::Highlight && !so->highlights_.empty())
            so->output->schedule_frame();
        // Enabled again: the gamma table is sent again.
        if (so->scene->gamma_ && (st->committed & backend::OutputState::Enabled) && !so->output->enabled)
            so->gamma_lut_changed_ = true;
        // What was shown with this frame waits to hear when.
        if (st->committed & backend::OutputState::Buffer) {
            for (Feedbacks& f : so->sampled_)
                so->committed_.push_back({so->output->commit_seq, std::move(f)});
            so->sampled_.clear();
        }
    });
    so->present_ = output->events.present.connect([so](const backend::Present& p) {
        const backend::Present* e = &p;
        std::vector<Committed> due;
        std::erase_if(so->committed_, [&](Committed& c) {
            if (int32_t(c.seq - e->commit_seq) > 0)
                return false;
            due.push_back(std::move(c));
            return true;
        });
        if (!e->presented)
            return;  // dropped: they say "discarded"
        for (Committed& c : due)
            wl::Presentation::presented(std::move(c.feedbacks.list), so->global, e->when, uint32_t(e->refresh),
                                        e->seq, e->flags | (c.feedbacks.zero_copy ? uint32_t(wl::Presentation::ZeroCopy) : 0u));
    });
    so->damage_ = output->events.damage.connect([so](const pixman_region32_t* damage) {
        int w, h;
        so->output->transformed_resolution(&w, &h);
        pixman_region32_t d;
        pixman_region32_init(&d);
        pixman_region32_copy(&d, damage);
        wlr_region_transform(&d, &d, wlr_output_transform_invert(so->output->transform), w, h);
        so->damage(&d);
        pixman_region32_fini(&d);
    });
    so->needs_frame_ = output->events.needs_frame.connect([so] { so->output->schedule_frame(); });
    if (output->renderer) {
        // The effects buffers are the renderer's: gone before it is.
        so->renderer_destroy_.connect(&output->renderer->events.destroy, [so](void*) {
            so->fx_.release();
            so->warp_layers_.clear();
            if (so->lut_)
                so->lut_->tex = 0;  // went with its context
            so->renderer_destroy_.disconnect();
        });
    }
    so->update_geometry(false);
    return so;
}

void SceneOutput::destroy() {
    wl_signal_emit_mutable(&events.destroy, nullptr);
    SceneImpl::output_update(scene, &scene->outputs, this, nullptr);
    for (Highlight* h : highlights_) {
        pixman_region32_fini(&h->region);
        delete h;
    }
    commit_.disconnect();
    present_.disconnect();
    damage_.disconnect();
    needs_frame_.disconnect();
    output_destroy_.disconnect();
    wlr_damage_ring_finish(&damage_ring);
    pixman_region32_fini(&pending_commit_damage);
    wl_list_remove(&link);
    if (in_timeline_) {
        wlr_drm_syncobj_timeline_signal(in_timeline_, UINT64_MAX);
        wlr_drm_syncobj_timeline_unref(in_timeline_);
    }
    if (out_timeline_) {
        wlr_drm_syncobj_timeline_signal(out_timeline_, UINT64_MAX);
        wlr_drm_syncobj_timeline_unref(out_timeline_);
    }
    wlr_color_transform_unref(gamma_lut_transform_);
    drop_lut_texture();
    delete this;
}

void SceneOutput::drop_lut_texture() {
    if (!lut_ || !lut_->tex)
        return;
    if (render::Renderer* r = render::Renderer::from(output->renderer)) {
        r->egl().make_current();
        glDeleteTextures(1, &lut_->tex);
    }
    lut_->tex = 0;
}

void SceneOutput::set_color_lut(std::unique_ptr<render::ColorLut> lut) {
    drop_lut_texture();
    lut_ = std::move(lut);
    color_changed_ = true;
    damage_whole();
}

SceneOutput* Scene::output_for(const wl::Output* o) {
    SceneOutput* so;
    wl_list_for_each(so, &outputs, link)
        if (so->global == o)
            return so;
    return nullptr;
}

void SceneOutput::presentation_pending(std::vector<std::shared_ptr<void>> feedbacks, bool zero_copy) {
    if (!feedbacks.empty())
        sampled_.push_back({std::move(feedbacks), zero_copy});
}

SceneOutput* Scene::output_for(const backend::Output* o) {
    SceneOutput* so;
    wl_list_for_each(so, &outputs, link)
        if (so->output == o)
            return so;
    return nullptr;
}

// ---- damage and settings ------------------------------------------------

void SceneOutput::damage(const pixman_region32_t* d) {
    pixman_region32_t clipped;
    pixman_region32_init(&clipped);
    pixman_region32_intersect_rect(&clipped, d, 0, 0, unsigned(output->width), unsigned(output->height));
    if (pixman_region32_not_empty(&clipped)) {
        output->schedule_frame();
        wlr_damage_ring_add(&damage_ring, &clipped);
        pixman_region32_union(&pending_commit_damage, &pending_commit_damage, &clipped);
    }
    pixman_region32_fini(&clipped);
}

void SceneOutput::damage_whole() {
    pixman_region32_t d;
    pixman_region32_init_rect(&d, 0, 0, unsigned(output->width), unsigned(output->height));
    damage(&d);
    pixman_region32_fini(&d);
}

void SceneOutput::update_geometry(bool force) {
    damage_whole();
    SceneImpl::output_update(scene, &scene->outputs, nullptr, force ? this : nullptr);
}

void SceneOutput::set_position(int lx, int ly) {
    if (x == lx && y == ly)
        return;
    x = lx;
    y = ly;
    update_geometry(false);
}

void SceneOutput::set_sdr_white_nits(float nits) {
    if (sdr_white_nits_ == nits)
        return;
    sdr_white_nits_ = nits;
    color_changed_ = true;
    damage_whole();
}

void SceneOutput::set_sdr_primaries(const wlr_color_primaries* p) {
    const bool set = p != nullptr;
    if (set == sdr_primaries_set_ && (!set || std::memcmp(&sdr_primaries_, p, sizeof(*p)) == 0))
        return;
    sdr_primaries_set_ = set;
    if (set)
        sdr_primaries_ = *p;
    damage_whole();
}

void SceneOutput::set_tint(float r, float g, float b) {
    if (tint_[0] == r && tint_[1] == g && tint_[2] == b)
        return;
    tint_[0] = r;
    tint_[1] = g;
    tint_[2] = b;
    color_changed_ = true;
    damage_whole();
}

bool SceneOutput::needs_frame() const {
    return output->needs_frame || pixman_region32_not_empty(&pending_commit_damage) || gamma_lut_changed_;
}

void SceneOutput::send_frame_done(const timespec* now) {
    SceneImpl::send_frame_done(scene, this, now);
}

void SceneOutput::for_each_buffer(const std::function<void(Buffer*, int, int)>& fn) {
    wlr_box box{x, y, 0, 0};
    output->effective_resolution(&box.width, &box.height);
    nodes_in_box(scene, box, [&](Node* n, const Walk& w) {
        if (n->type == Type::Buffer)
            fn(static_cast<Buffer*>(n), int(std::lround(w.x)), int(std::lround(w.y)));
        return false;
    });
}

// The colour work at the end of the frame: into the screen's primaries at
// its luminance for HDR, the night light's tint either way.
render::OutputColor SceneOutput::output_color(const backend::ImageDescription* desc) const {
    render::OutputColor c;
    float m[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    if (desc) {
        wlr_color_primaries srgb, dst;
        wlr_color_primaries_from_named(&srgb, WLR_COLOR_NAMED_PRIMARIES_SRGB);
        wlr_color_primaries_from_named(&dst, desc->primaries);
        wlr_color_primaries_transform_absolute_colorimetric(&srgb, &dst, m);
        const wlr_color_luminances srgb_lum = render::default_luminance(WLR_COLOR_TRANSFER_FUNCTION_SRGB);
        const wlr_color_luminances dst_lum = render::default_luminance(desc->transfer_function);
        float lum = luminance_multiplier(srgb_lum, dst_lum);
        // 1.0 (SDR white) is sdr_white_nits of the output's range.
        if (sdr_white_nits_ > 0)
            lum = sdr_white_nits_ / dst_lum.max;
        for (float& v : m)
            v *= lum;
        c.tf = tf_index(desc->transfer_function);
    }
    // The display's profile, on SDR: the table expects gamma 2.2 in.
    if (lut_ && lut_->size > 1 && (!desc || c.tf == 0 || c.tf == 3)) {
        c.lut = lut_.get();
        c.tf = 0;
    }
    // Columns take the tint: it applies to linear sRGB in.
    for (int i = 0; i < 9; ++i)
        m[i] *= tint_[i % 3] > 0 ? tint_[i % 3] : 1.0f;
    std::memcpy(c.matrix, m, sizeof(m));
    return c;
}

void SceneOutput::attempt_gamma(backend::OutputState* state) {
    if (!gamma_lut_changed_)
        return;
    backend::OutputState pending = *state;
    pending.set_color_transform(gamma_lut_transform_);
    gamma_lut_changed_ = false;
    if (!output->test_state(pending)) {
        if (scene->gamma_ && global)
            scene->gamma_->fail(global);
        wlr_color_transform_unref(gamma_lut_transform_);
        gamma_lut_transform_ = nullptr;
        return;
    }
    *state = pending;
}

bool SceneOutput::commit(const StateOptions* options) {
    if (!needs_frame())
        return true;
    backend::OutputState state;
    bool ok = build_state(&state, options) && output->commit_state(state);
    return ok;
}

// ---- Timer ----------------------------------------------------------------

int64_t Timer::duration_ns() {
    if (!render_timer)
        return pre_render_duration;
    const int64_t r = wlr_render_timer_get_duration_ns(render_timer);
    return r != -1 ? pre_render_duration + r : -1;
}

void Timer::finish() {
    if (render_timer)
        wlr_render_timer_destroy(render_timer);
    render_timer = nullptr;
}

// ---- drawing ----------------------------------------------------------------

namespace {

} // namespace

// A warped tree: everything in it (shadow, outline, rounded buffers, title
// bar) drawn as it is into an offscreen layer, then the layer drawn once
// through the warp. Only on outputs that aren't rotated.
void SceneImpl::render_warp_layer(Tree* tree, const Walk& w, RenderData& d, Scene* scene, render::RenderPass* pass,
                                  wlr_renderer* renderer, wlr_drm_syncobj_timeline* in_timeline, uint64_t in_point) {
    const wlr_fbox& frame = tree->warp_frame();
    if (d.transform != WL_OUTPUT_TRANSFORM_NORMAL || frame.width <= 0 || frame.height <= 0)
        return;
    // What's in it, bottom to top, as if it weren't warped. Blur can't be
    // drawn off screen (it samples what's behind): it sits the warp out.
    std::vector<Entry> entries;
    pixman_region32_t area;
    pixman_region32_init(&area);
    const std::function<void(Node*, const Walk&)> collect = [&](Node* n, const Walk& nw) {
        if (!n->enabled)
            return;
        if (n->type == Type::Tree) {
            Tree* t = static_cast<Tree*>(n);
            for (Node* child : each_child(t)) {
                Walk cw = nw.child(t, child);
                cw.warp = nullptr;
                collect(child, cw);
            }
            return;
        }
        if (n->type == Type::Blur || n->type == Type::BlurCache || SceneImpl::invisible(n))
            return;
        entries.push_back({n, nw});
        const wlr_box b = box_of(n, nw);
        pixman_region32_union_rect(&area, &area, b.x, b.y, unsigned(b.width), unsigned(b.height));
    };
    Walk tw = w;
    tw.warp = nullptr;
    collect(tree, tw);
    const pixman_box32_t* ext = pixman_region32_extents(&area);
    const wlr_box bounds{ext->x1, ext->y1, ext->x2 - ext->x1, ext->y2 - ext->y1};
    pixman_region32_fini(&area);
    if (entries.empty() || bounds.width <= 0 || bounds.height <= 0)
        return;

    SceneOutput* out = d.output;
    auto& layer = out->warp_layers_[tree];
    if (!layer)
        layer = std::make_unique<render::Target>();
    out->warp_layers_used_.insert(tree);
    const int lw = int(std::ceil(bounds.width * d.scale)), lh = int(std::ceil(bounds.height * d.scale));
    if (!pass->push_target(*layer, lw, lh))
        return;
    RenderData ld;
    ld.transform = WL_OUTPUT_TRANSFORM_NORMAL;
    ld.scale = d.scale;
    ld.logical = bounds;
    ld.trans_width = lw;
    ld.trans_height = lh;
    ld.output = out;
    ld.pass = pass;
    ld.whole = true;
    pixman_region32_init_rect(&ld.damage, 0, 0, unsigned(lw), unsigned(lh));
    for (const Entry& e : entries)
        render_entry(e, ld, scene, pass, renderer, in_timeline, in_point);
    pixman_region32_fini(&ld.damage);
    pass->pop_target();

    const double u0 = (bounds.x - frame.x) / frame.width, u1 = (bounds.x + bounds.width - frame.x) / frame.width;
    const double v0 = (bounds.y - frame.y) / frame.height, v1 = (bounds.y + bounds.height - frame.y) / frame.height;
    const auto verts = warp::mesh(tree->warp(), u0, v0, u1, v1, 16);
    std::vector<render::RenderPass::MeshVertex> mesh;
    mesh.reserve(verts.size());
    for (const warp::Vertex& v : verts)
        mesh.push_back({float((v.x - d.logical.x) * d.scale), float((v.y - d.logical.y) * d.scale), v.u, v.v});
    render::TextureDraw td;
    td.tex = layer->get()->texture();
    pass->add_texture_mesh(td, mesh);
}

void SceneImpl::render_entry(const Entry& e, RenderData& d, Scene* scene, render::RenderPass* pass,
                             wlr_renderer* renderer, wlr_drm_syncobj_timeline* in_timeline, uint64_t in_point) {
    Node* node = e.node;
    const Walk& w = e.walk;
    if (node->type == Type::Tree) {
        Tree* t = static_cast<Tree*>(node);
        if (t->warp())
            render_warp_layer(t, w, d, scene, pass, renderer, in_timeline, in_point);
        return;
    }

    pixman_region32_t region;
    pixman_region32_init(&region);
    if (d.whole) {
        const wlr_box b = box_of(node, w);
        pixman_region32_init_rect(&region, b.x, b.y, unsigned(b.width), unsigned(b.height));
    } else {
        pixman_region32_copy(&region, &node->visible);
    }
    pixman_region32_translate(&region, -d.logical.x, -d.logical.y);
    logical_to_buffer(&region, d, true);
    pixman_region32_intersect(&region, &region, &d.damage);
    if (!pixman_region32_not_empty(&region)) {
        pixman_region32_fini(&region);
        return;
    }
    const render::FBox dst = to_buffer(fbox_of(node, w), d);
    const wl_output_transform ot = d.transform;
    const double scale = w.scale * d.scale;

    switch (node->type) {
    case Type::Tree:
        break;
    case Type::Rect: {
        Rect* r = static_cast<Rect*>(node);
        render::RectDraw rd;
        rd.box = dst;
        for (int i = 0; i < 4; ++i)
            rd.color[i] = r->color[i] * w.opacity;
        rd.clip = &region;
        rd.corners = corners_of(r->corners, ot, scale);
        rd.cut = cut_of(r->cut, w, d, ot);
        pass->add_rect(rd);
        break;
    }
    case Type::Buffer: {
        Buffer* b = static_cast<Buffer*>(node);
        const float alpha = b->opacity * w.opacity;
        if (b->single_pixel_) {
            // Drawn as a rectangle, which is cheaper.
            render::RectDraw rd;
            rd.box = dst;
            const float a = b->single_pixel_color_[3] * alpha;
            for (int i = 0; i < 3; ++i)
                rd.color[i] = b->single_pixel_color_[i] * (alpha);
            rd.color[3] = a;
            rd.clip = &region;
            pass->add_rect(rd);
            break;
        }
        wlr_texture* tex = SceneImpl::texture(b, renderer);
        render::Texture* t = render::Renderer::texture(tex);
        if (!t) {
            SceneImpl::output_damage(d.output, &region);
            break;
        }
        const wl_output_transform transform =
            wlr_output_transform_compose(wlr_output_transform_invert(b->transform), ot);

        wlr_color_primaries primaries{};
        if (b->primaries)
            wlr_color_primaries_from_named(&primaries, b->primaries);
        // The content's reference white lands on SDR white (1.0) whatever
        // its transfer function; the output shows 1.0 at the user's SDR
        // brightness, so HDR scales with it (KWin does the same).
        const float lum = luminance_multiplier(render::default_luminance(b->transfer_function),
                                               render::default_luminance(WLR_COLOR_TRANSFER_FUNCTION_SRGB));

        pixman_region32_t opaque;
        pixman_region32_init(&opaque);
        SceneImpl::opaque_region(node, Walk{w.x - d.logical.x, w.y - d.logical.y, w.scale, w.opacity}, &opaque);
        logical_to_buffer(&opaque, d, false);
        pixman_region32_subtract(&opaque, &region, &opaque);

        render::TextureDraw td;
        td.tex = t->ref();
        td.src = b->src_box;
        td.dst = dst;
        td.transform = transform;
        td.clip = &region;
        td.alpha = alpha;
        td.filter = b->filter_mode;
        td.blend = !scene->calculate_visibility || pixman_region32_not_empty(&opaque);
        td.transfer = b->transfer_function;
        td.primaries = b->primaries ? &primaries : nullptr;
        td.luminance = lum;
        td.wait_timeline = b->wait_timeline_;
        td.wait_point = b->wait_point_;
        td.corners = corners_of(b->corners, transform, scale);
        pixman_region32_fini(&opaque);
        // SDR content spread over a wider gamut on an HDR output.
        const wlr_color_primaries* sdr = d.output->sdr_primaries_set_ && d.output->sdr_white_nits_ > 0 &&
                                                 b->primaries == 0 &&
                                                 (b->transfer_function == 0 ||
                                                  b->transfer_function == WLR_COLOR_TRANSFER_FUNCTION_SRGB ||
                                                  b->transfer_function == WLR_COLOR_TRANSFER_FUNCTION_GAMMA22)
                                             ? &d.output->sdr_primaries_
                                             : nullptr;
        if (!td.primaries)
            td.primaries = sdr;
        pass->add_texture(td);

        OutputSampleEvent ev{d.output, false, in_timeline, in_point};
        wl_signal_emit_mutable(&b->events.output_sample, &ev);
        break;
    }
    case Type::Shadow: {
        Shadow* s = static_cast<Shadow*>(node);
        render::ShadowDraw sd;
        sd.box = dst;
        for (int i = 0; i < 4; ++i)
            sd.color[i] = s->color[i];
        sd.color[3] *= w.opacity;
        sd.sigma = s->blur_sigma * float(scale);
        sd.radius = s->corner_radius * float(scale);
        sd.clip = &region;
        sd.cut = cut_of(s->cut, w, d, ot);
        pass->add_shadow(sd);
        break;
    }
    case Type::BlurCache: {
        BlurCache* c = static_cast<BlurCache*>(node);
        if (c->dirty && scene->blur().enabled() && pass->render_blur_cache(dst)) {
            c->dirty = false;
            scene->blur_cache_rendered_ = true;
        }
        break;
    }
    case Type::Blur: {
        Blur* bl = static_cast<Blur*>(node);
        render::BlurDraw bd;
        bd.box = dst;
        bd.clip = &region;
        bd.corners = corners_of(bl->corners, ot, scale);
        bd.cut = cut_of(bl->cut, w, d, ot);
        bd.alpha = bl->alpha * w.opacity;
        bd.strength = bl->strength;
        bd.use_cache = bl->use_cache;
        render::TexRef mask_ref;
        if (Buffer* m = bl->mask()) {
            if (render::Texture* mt = render::Renderer::texture(SceneImpl::texture(m, renderer))) {
                mask_ref = mt->ref();
                bd.mask = &mask_ref;
                bd.mask_src = m->src_box;
                bd.mask_transform = wlr_output_transform_compose(wlr_output_transform_invert(m->transform), ot);
                // The mask's place, relative to the blur's.
                bd.mask_box = to_buffer(fbox_of(m, Walk{w.x + (m->x - bl->x) * w.scale,
                                                        w.y + (m->y - bl->y) * w.scale, w.scale, w.opacity}),
                                        d);
            }
        }
        bd.refraction = bl->refraction * float(scale);
        bd.thickness = bl->thickness * float(scale);
        bd.glass = bl->glass;
        render::GlassShapeDraw shapes[kMaxGlassShapes];
        for (int i = 0; i < bl->shape_count; ++i) {
            const GlassShape& s = bl->shapes[i];
            render::GlassShapeDraw& o = shapes[i];
            // Exact to the subpixel, so shapes move smoothly.
            const render::FBox b =
                to_buffer({w.x + s.x * w.scale, w.y + s.y * w.scale, s.width * w.scale, s.height * w.scale}, d);
            o.x = float(b.x);
            o.y = float(b.y);
            o.width = float(b.width);
            o.height = float(b.height);
            o.radius = s.radius * float(scale);
            o.opacity = s.opacity;
            if (s.clip_width > 0 && s.clip_height > 0) {
                const render::FBox c = to_buffer({std::floor(w.x + s.clip_x * w.scale),
                                                  std::floor(w.y + s.clip_y * w.scale),
                                                  std::ceil(s.clip_width * w.scale), std::ceil(s.clip_height * w.scale)},
                                                 d);
                o.clip_x = float(c.x);
                o.clip_y = float(c.y);
                o.clip_width = float(c.width);
                o.clip_height = float(c.height);
            } else {
                o.clip_x = o.clip_y = 0;
                o.clip_width = o.clip_height = -1;
            }
        }
        bd.shapes = shapes;
        bd.shape_count = bl->shape_count;
        pass->add_blur(bd);
        break;
    }
    }
    pixman_region32_fini(&region);
}

namespace {

// Whether a blur node's redraw has to reach past the damage (artifacts
// otherwise), and with what blur.
bool blur_extends_damage(Node* node, render::BlurParams* params, bool* has_blur) {
    switch (node->type) {
    case Type::Blur: {
        Blur* b = static_cast<Blur*>(node);
        // Reading the cache at full strength: nothing to extend.
        if (b->use_cache && b->strength == 1.0f) {
            *has_blur = true;
            return false;
        }
        if (b->strength < 1.0f)
            *params = params->scaled(b->strength);
        break;
    }
    case Type::BlurCache:
        if (!static_cast<BlurCache*>(node)->dirty)
            return false;
        break;
    default:
        return false;
    }
    return params->enabled();
}

bool apply_blur_region(Node* node, const render::BlurParams& params, RenderData& d, backend::OutputState* state,
                       const pixman_region32_t* original, pixman_region32_t* padding) {
    const int reach = params.reach();
    pixman_region32_t visible;
    pixman_region32_init(&visible);
    pixman_region32_copy(&visible, &node->visible);
    pixman_region32_translate(&visible, -d.logical.x, -d.logical.y);
    logical_to_buffer(&visible, d, false);

    pixman_region32_t expanded;
    pixman_region32_init(&expanded);
    wlr_region_expand(&expanded, original, reach);
    pixman_region32_t isect;
    pixman_region32_init(&isect);
    bool hit = false;
    if (pixman_region32_intersect(&isect, &expanded, &visible) && pixman_region32_not_empty(&isect)) {
        hit = true;
        // Liquid Glass reads the backdrop far past the blur's kernel (its
        // lens shows what lies deeper in): it redraws whole. Its nodes are
        // small.
        if (node->type == Type::Blur) {
            Blur* b = static_cast<Blur*>(node);
            if (b->shape_count > 0 || b->refraction > 0)
                pixman_region32_copy(&isect, &visible);
        }
        pixman_region32_union(&d.damage, &d.damage, &isect);
        state->committed |= backend::OutputState::Damage;
        pixman_region32_union(&state->damage, &state->damage, &isect);
        // Once more, for the padding round it where the artifacts are.
        wlr_region_expand(&isect, &isect, reach);
        pixman_region32_subtract(&isect, &isect, padding);
        pixman_region32_union(padding, padding, &isect);
    }
    pixman_region32_fini(&isect);
    pixman_region32_fini(&expanded);
    pixman_region32_fini(&visible);
    return hit;
}

} // namespace

bool SceneOutput::build_state(backend::OutputState* state, const StateOptions* options) {
    const StateOptions none;
    if (!options)
        options = &none;
    Timer* timer = options->timer;
    timespec start;
    if (timer) {
        clock_gettime(CLOCK_MONOTONIC, &start);
        timer->finish();
        *timer = Timer{};
    }
    if ((state->committed & backend::OutputState::Enabled) && !state->enabled)
        return true;

    render::Renderer* renderer = render::Renderer::from(output->renderer);
    if (!renderer) {
        wlr_log(WLR_ERROR, "%s: not atrium's renderer", output->name);
        return false;
    }

    RenderData d{};
    d.transform = output->transform;
    d.scale = output->scale;
    d.logical = {x, y, 0, 0};
    d.output = this;
    int res_w, res_h;
    pending_resolution(output, state, &res_w, &res_h);
    if ((state->committed & backend::OutputState::Transform) && d.transform != state->transform) {
        damage_whole();
        d.transform = state->transform;
    }
    if ((state->committed & backend::OutputState::Scale) && d.scale != state->scale) {
        damage_whole();
        d.scale = state->scale;
    }
    d.trans_width = res_w;
    d.trans_height = res_h;
    wlr_output_transform_coords(d.transform, &d.trans_width, &d.trans_height);
    d.logical.width = int(d.trans_width / d.scale);
    d.logical.height = int(d.trans_height / d.scale);

    // What shows, top to bottom.
    std::vector<Entry> list;
    std::unordered_set<const Tree*> warped;
    const bool fractional = std::floor(d.scale) != d.scale;
    nodes_in_box(scene, d.logical, [&](Node* n, const Walk& w) {
        if (SceneImpl::invisible(n))
            return false;
        // Under a warp: the warped tree is one entry, drawn as a layer.
        if (w.warp) {
            Tree* t = const_cast<Tree*>(w.warp);
            if (warped.insert(t).second)
                list.push_back({t, walk_of(t)});
            return false;
        }
        // The background is black anyway: an opaque black rectangle (a
        // fullscreen app's backdrop) and what's under it needn't be drawn.
        if (scene->calculate_visibility && (!fractional || list.empty()) && w.identity()) {
            if (n->type == Type::Rect) {
                Rect* r = static_cast<Rect*>(n);
                if (r->color[0] == 0 && r->color[1] == 0 && r->color[2] == 0 && r->color[3] == 1 &&
                    r->corners.empty() && r->cut.empty())
                    return false;
            }
            if (n->type == Type::Buffer && SceneImpl::buffer_black_opaque(static_cast<Buffer*>(n)))
                return false;
        }
        pixman_region32_t isect;
        pixman_region32_init(&isect);
        pixman_region32_intersect_rect(&isect, &n->visible, d.logical.x, d.logical.y, unsigned(d.logical.width),
                                       unsigned(d.logical.height));
        const bool shows = pixman_region32_not_empty(&isect);
        pixman_region32_fini(&isect);
        if (shows)
            list.push_back({n, w});
        return false;
    });

    if (scene->debug_damage == Scene::DebugDamage::Rerender)
        damage_whole();
    timespec now{};
    if (scene->debug_damage == Scene::DebugDamage::Highlight) {
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (pixman_region32_not_empty(&damage_ring.current)) {
            auto* h = new Highlight;
            pixman_region32_init(&h->region);
            pixman_region32_copy(&h->region, &damage_ring.current);
            h->when = now;
            highlights_.insert(highlights_.begin(), h);
        }
        pixman_region32_t acc;
        pixman_region32_init(&acc);
        for (auto it = highlights_.begin(); it != highlights_.end();) {
            Highlight* h = *it;
            pixman_region32_subtract(&h->region, &h->region, &acc);
            pixman_region32_union(&acc, &acc, &h->region);
            if ((ns_of(now) - ns_of(h->when)) / 1000000 >= kHighlightFadeMs || !pixman_region32_not_empty(&h->region)) {
                pixman_region32_fini(&h->region);
                delete h;
                it = highlights_.erase(it);
            } else {
                ++it;
            }
        }
        damage(&acc);
        pixman_region32_fini(&acc);
    }

    state->set_damage(&pending_commit_damage);

    const backend::ImageDescription* desc = pending_image_description(output, state);
    const render::OutputColor color = output_color(desc);

    // Straight to the display: one buffer, nothing to adjust on the way
    // out (HDR's SDR white, colour intensity or a tint would be skipped).
    enum { Ineligible, Candidate, Success } scanout = Ineligible;
    const bool adjusts = sdr_white_nits_ > 0 || sdr_primaries_set_ || !color.plain() ||
                         (renderer && renderer->shaders().has_screen_shader());
    if (list.size() == 1 && !adjusts && scene->debug_damage != Scene::DebugDamage::Highlight &&
        scene->direct_scanout && list[0].node->type == Type::Buffer && list[0].walk.identity() &&
        !(state->committed & (backend::OutputState::ModeField | backend::OutputState::Enabled | backend::OutputState::RenderFormat)) &&
        output->direct_scanout_allowed()) {
        Buffer* b = static_cast<Buffer*>(list[0].node);
        if (b->buffer && b->transform == d.transform && scanout_colour_allowed(desc, b)) {
            scanout = Candidate;
            if (dmabuf_feedback_debounce_ >= kDmabufFeedbackDebounce && b->primary_output == this)
                send_dmabuf_feedback(scene, b, output);
            backend::OutputState pending = *state;
            {
                int dw = b->buffer->width, dh = b->buffer->height;
                wlr_output_transform_coords(b->transform, &dw, &dh);
                const wlr_fbox whole{0, 0, double(dw), double(dh)};
                if (!wlr_fbox_empty(&b->src_box) && !wlr_fbox_equal(&b->src_box, &whole))
                    pending.buffer_src_box = b->src_box;
                wlr_box dst{int(std::lround(list[0].walk.x)) - x, int(std::lround(list[0].walk.y)) - y, 0, 0};
                b->size(&dst.width, &dst.height);
                scale_box(&dst, d.scale);
                wlr_box_transform(&dst, &dst, wlr_output_transform_invert(d.transform), d.trans_width, d.trans_height);
                pending.buffer_dst_box = dst;
                wlr_buffer* wb = b->buffer;
                wl::SurfaceBuffer* sb = wl::SurfaceBuffer::from(wb);
                if (sb && sb->source.get() && sb->source.get()->n_locks > 0)
                    wb = sb->source.get();
                pending.set_buffer(wb);
                if (b->wait_timeline_)
                    pending.set_wait_timeline(b->wait_timeline_, b->wait_point_);
                if (out_timeline_) {
                    ++out_point_;
                    pending.set_signal_timeline(out_timeline_, out_point_);
                }
                if (output->test_state(pending)) {
                    *state = pending;
                    scanout = Success;
                    OutputSampleEvent ev{this, true, out_timeline_, out_point_};
                    wl_signal_emit_mutable(&b->events.output_sample, &ev);
                }
            }
        }
    }
    if (scanout == Ineligible) {
        if (dmabuf_feedback_debounce_ > 0)
            --dmabuf_feedback_debounce_;
    } else if (dmabuf_feedback_debounce_ < kDmabufFeedbackDebounce) {
        ++dmabuf_feedback_debounce_;
    }
    if ((scanout == Success) != prev_scanout_) {
        prev_scanout_ = scanout == Success;
        wlr_log(WLR_DEBUG, "Direct scan-out %s", prev_scanout_ ? "enabled" : "disabled");
    }
    if (scanout == Success) {
        attempt_gamma(state);
        if (timer) {
            timespec end;
            clock_gettime(CLOCK_MONOTONIC, &end);
            timer->pre_render_duration = ns_of(end) - ns_of(start);
        }
        return true;
    }

    backend::Swapchain* swapchain = options->swapchain;
    if (!swapchain) {
        if (!output->configure_primary_swapchain(state, output->swapchain))
            return false;
        swapchain = output->swapchain.get();
    }
    wlr_buffer* buffer = swapchain->acquire();
    if (!buffer)
        return false;
    assert(buffer->width == res_w && buffer->height == res_h);
    if (timer) {
        timer->render_timer = wlr_render_timer_create(output->renderer);
        timespec end;
        clock_gettime(CLOCK_MONOTONIC, &end);
        timer->pre_render_duration = ns_of(end) - ns_of(start);
    }
    if (color_changed_ || (state->committed & backend::OutputState::ImageDescriptionField)) {
        // A frame through the blend buffer starts afresh: all of it.
        color_changed_ = false;
        damage_whole();
        state->set_damage(&pending_commit_damage);
    }

    render::Framebuffer* fb = renderer->framebuffer_for(buffer);
    render::PassOptions po;
    po.effects = &fx_;
    po.timer = timer ? timer->render_timer : nullptr;
    po.color = color;
    ++in_point_;
    po.signal_timeline = in_timeline_;
    po.signal_point = in_point_;
    render::RenderPass* pass = fb ? renderer->begin(fb, po) : nullptr;
    if (!pass) {
        wlr_buffer_unlock(buffer);
        return false;
    }
    pass->blur_params = &scene->blur_;
    d.pass = pass;
    pixman_region32_init(&d.damage);
    wlr_damage_ring_rotate_buffer(&damage_ring, buffer, &d.damage);

    // Blur artifacts: a blur next to damage has to be redrawn where its
    // kernel reaches the damage, and the padding round that put back after.
    bool compensate = false;
    pixman_region32_t padding;
    pixman_region32_init(&padding);
    if (pass->effects() && pixman_region32_not_empty(&d.damage)) {
        pixman_region32_t original;
        pixman_region32_init(&original);
        pixman_region32_copy(&original, &d.damage);
        const pixman_box32_t* e = pixman_region32_extents(&original);
        const bool full = e->x2 - e->x1 >= output->width && e->y2 - e->y1 >= output->height;
        pixman_region32_t pad;
        pixman_region32_init(&pad);
        bool has_blur = false;
        for (auto it = list.rbegin(); it != list.rend(); ++it) {
            render::BlurParams params = scene->blur_;
            if (!blur_extends_damage(it->node, &params, &has_blur))
                continue;
            if (full) {
                compensate = false;
                break;
            }
            if (apply_blur_region(it->node, params, d, state, &original, &pad))
                compensate = true;
        }
        if (compensate) {
            pixman_region32_subtract(&padding, &pad, &d.damage);
            pixman_region32_intersect_rect(&padding, &padding, 0, 0, unsigned(output->width), unsigned(output->height));
            pixman_region32_union(&d.damage, &d.damage, &padding);
            pixman_region32_intersect_rect(&d.damage, &d.damage, 0, 0, unsigned(output->width), unsigned(output->height));
            // The content round the blur, as it is, to paste over the
            // artifacts after (else what's drawn over the blur would leak
            // into it next frame).
            pass->copy(&padding, pass->effects()->saved.get(), pass->target());
        }
        pixman_region32_fini(&pad);
        pixman_region32_fini(&original);
    }

    // The background (black), where nothing opaque covers it.
    pixman_region32_t background;
    pixman_region32_init(&background);
    pixman_region32_copy(&background, &d.damage);
    if (scene->calculate_visibility) {
        for (auto it = list.rbegin(); it != list.rend(); ++it) {
            pixman_region32_t opaque;
            pixman_region32_init(&opaque);
            SceneImpl::opaque_region(it->node, it->walk, &opaque);
            pixman_region32_intersect(&opaque, &opaque, &it->node->visible);
            pixman_region32_translate(&opaque, -x, -y);
            logical_to_buffer(&opaque, d, false);
            pixman_region32_subtract(&background, &background, &opaque);
            pixman_region32_fini(&opaque);
        }
        if (fractional) {
            wlr_region_expand(&background, &background, 1);
            pixman_region32_intersect(&background, &background, &d.damage);
        }
    }
    render::RectDraw bg;
    bg.box = {0, 0, double(buffer->width), double(buffer->height)};
    bg.clip = &background;
    bg.blend = false;
    pass->add_rect(bg);
    pixman_region32_fini(&background);

    warp_layers_used_.clear();
    for (auto it = list.rbegin(); it != list.rend(); ++it) {
        SceneImpl::render_entry(*it, d, scene, pass, output->renderer, in_timeline_, in_point_);
        if (it->node->type == Type::Buffer) {
            Buffer* b = static_cast<Buffer*>(it->node);
            // Composited: feedback for composition once direct scan-out has
            // stopped being tried.
            if (dmabuf_feedback_debounce_ == 0 && b->primary_output == this)
                send_dmabuf_feedback(scene, b, nullptr);
        }
    }
    // Layers of trees no longer warped go.
    std::erase_if(warp_layers_, [this](const auto& kv) { return !warp_layers_used_.contains(kv.first); });

    if (scene->debug_damage == Scene::DebugDamage::Highlight) {
        for (Highlight* h : highlights_) {
            const float a = 1.0f - float(ns_of(now) - ns_of(h->when)) / 1e6f / kHighlightFadeMs;
            render::RectDraw rd;
            rd.box = {0, 0, double(buffer->width), double(buffer->height)};
            rd.color[0] = a * 0.5f;
            rd.color[1] = rd.color[2] = 0;
            rd.color[3] = a * 0.5f;
            rd.clip = &h->region;
            pass->add_rect(rd);
        }
    }

    pass->apply_screen_shader(&d.damage);
    if (scene->draw_cursor)
        scene->draw_cursor(output, pass->wlr(), &d.damage);
    if (compensate)
        pass->copy(&padding, pass->target(), pass->effects()->saved.get());
    pixman_region32_fini(&padding);
    pixman_region32_fini(&d.damage);

    if (!pass->submit()) {
        wlr_buffer_unlock(buffer);
        // The buffer's contents are undefined now.
        wlr_damage_ring_add_whole(&damage_ring);
        return false;
    }
    state->set_buffer(buffer);
    wlr_buffer_unlock(buffer);
    if (in_timeline_) {
        state->set_wait_timeline(in_timeline_, in_point_);
        ++out_point_;
        state->set_signal_timeline(out_timeline_, out_point_);
    }
    attempt_gamma(state);

    // While it was dirty, a BlurCache kept everything below it visible to
    // blur it again. Content-only commits (a fullscreen game) never
    // recompute visibility: without this, what's under an opaque window
    // would stay in the list, drawn every frame, blocking direct scan-out.
    if (scene->blur_cache_rendered_) {
        scene->blur_cache_rendered_ = false;
        pixman_region32_t r;
        pixman_region32_init_rect(&r, x, y, unsigned(d.logical.width), unsigned(d.logical.height));
        SceneImpl::update_region(scene, &r);
        pixman_region32_fini(&r);
    }
    return true;
}

} // namespace atrium::scene
