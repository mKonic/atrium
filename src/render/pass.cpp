#include "render/pass.hpp"
#include "util/log.hpp"

#include "render/matrix.hpp"

#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace atrium::render {

namespace {

constexpr int kMaxQuads = 86;

// Whole pixels covering a float box.
pixman_region32_t region_of(const FBox& b) {
    pixman_region32_t r;
    const int x1 = int(std::floor(b.x)), y1 = int(std::floor(b.y));
    const int x2 = int(std::ceil(b.x + b.width)), y2 = int(std::ceil(b.y + b.height));
    pixman_region32_init_rect(&r, x1, y1, unsigned(std::max(0, x2 - x1)), unsigned(std::max(0, y2 - y1)));
    return r;
}

void set_corners(Program& p, const char* tl, const char* tr, const char* bl, const char* br, const Corners& c) {
    p.set(tl, c.tl);
    p.set(tr, c.tr);
    p.set(bl, c.bl);
    p.set(br, c.br);
}

void set_cut(Program& p, const CutOut& c) {
    p.set("clip_size", float(c.area.width), float(c.area.height));
    p.set("clip_position", float(c.area.x), float(c.area.y));
    set_corners(p, "clip_radius_top_left", "clip_radius_top_right", "clip_radius_bottom_left",
                "clip_radius_bottom_right", c.corners);
}

// The cut-out's inside needn't be drawn at all: all but a margin where its
// corners curve comes off the clip.
void cut_from_clip(pixman_region32_t* clip, const CutOut& cut) {
    if (!cut.valid())
        return;
    const Corners& c = cut.corners;
    const float top = std::max(c.tl, c.tr), bottom = std::max(c.bl, c.br);
    const float left = std::max(c.tl, c.bl), right = std::max(c.tr, c.br);
    pixman_region32_t inner;
    pixman_region32_init_rect(&inner, int(cut.area.x + left * 0.3f), int(cut.area.y + top * 0.3f),
                              unsigned(std::max(0.0f, cut.area.width - (left + right) * 0.3f)),
                              unsigned(std::max(0.0f, cut.area.height - (top + bottom) * 0.3f)));
    pixman_region32_subtract(clip, clip, &inner);
    pixman_region32_fini(&inner);
}

void set_tex_matrix(Program& p, wl_output_transform t, const wlr_fbox& box) {
    float m[9];
    matrix::identity(m);
    matrix::translate(m, float(box.x), float(box.y));
    matrix::scale(m, float(box.width), float(box.height));
    matrix::translate(m, 0.5f, 0.5f);
    // Textures' origin differs, so rotations go the other way.
    matrix::transform(m, (t & WL_OUTPUT_TRANSFORM_90) ? wlr_output_transform_invert(t) : t);
    matrix::translate(m, -0.5f, -0.5f);
    p.set_mat3_raw("tex_proj", m);
}

void stencil_begin() {
    glClearStencil(0);
    glClear(GL_STENCIL_BUFFER_BIT);
    glEnable(GL_STENCIL_TEST);
    glStencilFunc(GL_ALWAYS, 1, 0xFF);
    glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
}

void stencil_inside() {
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glStencilFunc(GL_EQUAL, 1, 0xFF);
    glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
}

void stencil_end() {
    glClearStencil(0);
    glClear(GL_STENCIL_BUFFER_BIT);
    glDisable(GL_STENCIL_TEST);
}

void filter(GLenum target, wlr_scale_filter_mode mode) {
    const GLint f = mode == WLR_SCALE_FILTER_NEAREST ? GL_NEAREST : GL_LINEAR;
    glTexParameteri(target, GL_TEXTURE_MIN_FILTER, f);
    glTexParameteri(target, GL_TEXTURE_MAG_FILTER, f);
}

int tf_index(wlr_color_transfer_function tf) {
    switch (tf) {
    case WLR_COLOR_TRANSFER_FUNCTION_ST2084_PQ:
        return 1;
    case WLR_COLOR_TRANSFER_FUNCTION_EXT_LINEAR:
        return 2;
    default:
        return 0;
    }
}

} // namespace

int BlurParams::reach() const {
    return int(std::pow(2, passes + 1) * radius);
}

BlurParams BlurParams::scaled(float strength) const {
    BlurParams b = *this;
    auto lerp = [](float a, float c, float f) { return a * (1 - f) + c * f; };
    const float s = std::clamp(strength, 0.0f, 1.0f);
    b.brightness = lerp(1, brightness, s);
    b.contrast = lerp(1, contrast, s);
    b.noise = lerp(0, noise, s);
    b.saturation = lerp(1, saturation, s);
    if (s <= 0) {
        b.passes = 0;
        b.radius = 0;
    } else if (s < 1) {
        // The same reach, scaled, never past the full settings.
        const int size = int(reach() * s);
        b.passes = int(std::max(0.0f, std::min(float(passes), std::ceil(std::log2(size / radius)))));
        b.radius = std::max(0.0f, std::min(radius, float(size / std::pow(2, b.passes + 1))));
    }
    return b;
}

// ---- lifetime -----------------------------------------------------------

RenderPass* Renderer::begin(Framebuffer* fb, const PassOptions& o) {
    if (!egl_->make_current() || check_reset() || !fb->get_fbo())
        return nullptr;
    shaders_->poll_reload();
    return new RenderPass(*this, fb, o);
}

namespace {

// RenderPass::Hook's layout: the wlr_render_pass, then its owner.
struct PassHook {
    wlr_render_pass base;
    RenderPass* self;
};

RenderPass* pass_of(wlr_render_pass* p) { return reinterpret_cast<PassHook*>(p)->self; }

bool wlr_submit(wlr_render_pass* p) { return pass_of(p)->submit(); }

void wlr_add_texture(wlr_render_pass* p, const wlr_render_texture_options* o) {
    Texture* t = Renderer::texture(o->texture);
    if (!t)
        return;
    wlr_fbox src;
    wlr_box dst;
    wlr_render_texture_options_get_src_box(o, &src);
    wlr_render_texture_options_get_dst_box(o, &dst);
    TextureDraw d;
    d.tex = t->ref();
    d.src = src;
    d.dst = FBox::of(dst);
    d.transform = o->transform;
    d.clip = o->clip;
    d.alpha = wlr_render_texture_options_get_alpha(o);
    d.filter = o->filter_mode;
    d.blend = o->blend_mode != WLR_RENDER_BLEND_MODE_NONE;
    d.transfer = o->transfer_function;
    d.primaries = o->primaries;
    d.luminance = o->luminance_multiplier ? *o->luminance_multiplier : 1;
    d.wait_timeline = o->wait_timeline;
    d.wait_point = o->wait_point;
    pass_of(p)->add_texture(d);
}

void wlr_add_rect(wlr_render_pass* p, const wlr_render_rect_options* o) {
    RenderPass* pass = pass_of(p);
    RectDraw d;
    d.box = FBox::of(o->box);
    d.color[0] = o->color.r;
    d.color[1] = o->color.g;
    d.color[2] = o->color.b;
    d.color[3] = o->color.a;
    d.clip = o->clip;
    d.blend = o->blend_mode != WLR_RENDER_BLEND_MODE_NONE;
    pass->add_rect(d);
}

const wlr_render_pass_impl kPassImpl = {
    .submit = wlr_submit,
    .add_texture = wlr_add_texture,
    .add_rect = wlr_add_rect,
};

} // namespace

RenderPass* RenderPass::from(wlr_render_pass* p) {
    return p && p->impl == &kPassImpl ? pass_of(p) : nullptr;
}

RenderPass::RenderPass(Renderer& r, Framebuffer* fb, const PassOptions& o)
    : r_(r), fb_(fb), output_fb_(nullptr), fx_(o.effects), width_(fb->width), height_(fb->height),
      timer_(reinterpret_cast<RenderTimer*>(o.timer)), color_(o.color) {
    wlr_render_pass_init(&hook_.base, &kPassImpl);
    hook_.self = this;
    if (fb->buffer)
        locked_ = wlr_buffer_lock(fb->buffer);
    fb->encoded_tf = 0;  // until the colour pass says otherwise
    if (o.signal_timeline) {
        signal_timeline_ = wlr_drm_syncobj_timeline_ref(o.signal_timeline);
        signal_point_ = o.signal_point;
    }
    if (timer_)
        clock_gettime(CLOCK_MONOTONIC, &timer_->cpu_start);
    matrix::projection(proj_, width_, height_, WL_OUTPUT_TRANSFORM_FLIPPED_180);

    // Effects buffers the frame's size, half float for HDR (blurring an
    // HDR frame in 8 bits would clip it at SDR white).
    two_pass_ = !color_.plain() && r.caps().EXT_color_buffer_half_float;
    if (fx_) {
        const GLenum fmt = two_pass_ ? GL_RGBA16F : GL_RGBA8;
        bool ok = fx_->effects.ensure(r, width_, height_, fmt) &&
                  fx_->effects_swapped.ensure(r, width_, height_, fmt) &&
                  fx_->cache_blurred.ensure(r, width_, height_, fmt) &&
                  fx_->cache_plain.ensure(r, width_, height_, fmt) && fx_->saved.ensure(r, width_, height_, fmt);
        if (!ok) {
            alog(Log::Error, "Couldn't allocate effects buffers: no blur");
            fx_ = nullptr;
        }
    }
    if (two_pass_) {
        Target& blend = fx_ ? fx_->blend : own_blend_;
        if (blend.ensure(r, width_, height_, GL_RGBA16F)) {
            output_fb_ = fb_;
            fb_ = blend.get();
        } else {
            two_pass_ = false;
        }
    }

    bind(fb_);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_STENCIL_TEST);
}

RenderPass::~RenderPass() {
    if (signal_timeline_)
        wlr_drm_syncobj_timeline_unref(signal_timeline_);
    if (locked_)
        wlr_buffer_unlock(locked_);
}

bool RenderPass::submit() {
    r_.egl().make_current();
    if (two_pass_ && output_fb_)
        output_pass();

    if (timer_) {
        GLint64 disjoint = 0;
        r_.procs.glGetInteger64vEXT(GL_GPU_DISJOINT_EXT, &disjoint);  // clears the flag
        r_.procs.glQueryCounterEXT(timer_->query, GL_TIMESTAMP_EXT);
        r_.procs.glGetInteger64vEXT(GL_TIMESTAMP_EXT, &timer_->gl_cpu_end);
        clock_gettime(CLOCK_MONOTONIC, &timer_->cpu_end);
    }

    bool ok = true;
    if (signal_timeline_) {
        ok = false;
        EGLSyncKHR sync = r_.egl().create_sync(-1);
        if (sync != EGL_NO_SYNC_KHR) {
            int fd = r_.egl().dup_fence_fd(sync);
            r_.egl().destroy_sync(sync);
            if (fd >= 0) {
                ok = wlr_drm_syncobj_timeline_import_sync_file(signal_timeline_, signal_point_, fd);
                close(fd);
            }
        }
    } else {
        glFlush();
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    delete this;
    return ok;
}

// ---- helpers ------------------------------------------------------------

void RenderPass::bind(Framebuffer* fb) {
    glBindFramebuffer(GL_FRAMEBUFFER, fb->get_fbo());
    glViewport(0, 0, fb->width, fb->height);
}

TexRef RenderPass::sampler(Framebuffer* fb) {
    if (fb->buffer && !fb->external_only) {
        // Bound to the image again on every read, as texture_from_buffer
        // does: a texture made once needn't see what was drawn into the
        // buffer since (NVIDIA's scanout buffers keep showing their old
        // contents, and the blur and its padding read those).
        const bool fresh = !fb->tex;
        if (fresh)
            glGenTextures(1, &fb->tex);
        glBindTexture(GL_TEXTURE_2D, fb->tex);
        if (fresh) {
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        }
        r_.procs.glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, fb->image);
        glBindTexture(GL_TEXTURE_2D, 0);
    }
    return fb->texture();
}

void RenderPass::set_proj(Program& p, const FBox& box) {
    float m[9];
    matrix::identity(m);
    matrix::translate(m, float(box.x), float(box.y));
    matrix::scale(m, float(box.width), float(box.height));
    matrix::multiply(m, proj_, m);
    p.set_mat3_raw("proj", m);
}

void RenderPass::draw(Program& p, const FBox& box, const pixman_region32_t* clip) {
    if (box.empty() || !p.id)
        return;
    p.set("frag_size", float(width_), float(height_));
    pixman_region32_t region = region_of(box);
    if (clip)
        pixman_region32_intersect(&region, &region, clip);
    int n = 0;
    const pixman_box32_t* rects = pixman_region32_rectangles(&region, &n);
    if (n > 0) {
        glEnableVertexAttribArray(GLuint(p.pos));
        GLfloat verts[kMaxQuads * 12];
        const double w = box.width, h = box.height;
        for (int i = 0; i < n;) {
            const int batch = std::min(n - i, kMaxQuads);
            size_t k = 0;
            for (int j = 0; j < batch; ++j, ++i) {
                const float x1 = float((rects[i].x1 - box.x) / w), y1 = float((rects[i].y1 - box.y) / h);
                const float x2 = float((rects[i].x2 - box.x) / w), y2 = float((rects[i].y2 - box.y) / h);
                const float q[12] = {x1, y1, x2, y1, x1, y2, x2, y1, x2, y2, x1, y2};
                std::memcpy(verts + k, q, sizeof(q));
                k += 12;
            }
            glVertexAttribPointer(GLuint(p.pos), 2, GL_FLOAT, GL_FALSE, 0, verts);
            glDrawArrays(GL_TRIANGLES, 0, batch * 6);
        }
        glDisableVertexAttribArray(GLuint(p.pos));
    }
    pixman_region32_fini(&region);
}

// ---- drawing ------------------------------------------------------------

void RenderPass::add_texture_mesh(const TextureDraw& d, const std::vector<MeshVertex>& vertices) {
    if (vertices.empty() || !d.tex.tex)
        return;
    const int source = d.tex.target == GL_TEXTURE_EXTERNAL_OES ? 2 : d.tex.has_alpha ? 0 : 1;
    Program& p = r_.shaders().get(Shader::Tex, 6 + source);
    if (!p.id || p.at < 0)
        return;
    wlr_fbox src = d.src;
    if (src.width <= 0 || src.height <= 0)
        src = {0, 0, double(d.tex.width), double(d.tex.height)};
    src.x /= d.tex.width;
    src.y /= d.tex.height;
    src.width /= d.tex.width;
    src.height /= d.tex.height;

    glEnable(GL_BLEND);
    glUseProgram(p.id);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(d.tex.target, d.tex.tex);
    filter(d.tex.target, WLR_SCALE_FILTER_BILINEAR);
    p.set("tex", 0);
    p.set("alpha", d.alpha);
    p.set("discard_transparent", 0);
    float prim[9];
    matrix::identity(prim);
    p.set("hdr_tf", 0);
    p.set_mat3("hdr_prim", prim);
    p.set("hdr_lum", 1.0f);
    set_proj(p, FBox{0, 0, 1, 1});  // `at` is in framebuffer pixels already
    set_tex_matrix(p, d.transform, src);
    p.set("frag_size", float(width_), float(height_));

    std::vector<GLfloat> pos, at;
    pos.reserve(vertices.size() * 2);
    at.reserve(vertices.size() * 2);
    for (const MeshVertex& v : vertices) {
        pos.push_back(v.u);
        pos.push_back(v.v);
        at.push_back(v.x);
        at.push_back(v.y);
    }
    glEnableVertexAttribArray(GLuint(p.pos));
    glEnableVertexAttribArray(GLuint(p.at));
    glVertexAttribPointer(GLuint(p.pos), 2, GL_FLOAT, GL_FALSE, 0, pos.data());
    glVertexAttribPointer(GLuint(p.at), 2, GL_FLOAT, GL_FALSE, 0, at.data());
    glDrawArrays(GL_TRIANGLES, 0, GLsizei(vertices.size()));
    glDisableVertexAttribArray(GLuint(p.at));
    glDisableVertexAttribArray(GLuint(p.pos));
    glBindTexture(d.tex.target, 0);
}

void RenderPass::add_texture(const TextureDraw& d) {
    if (d.dst.empty() || !d.tex.tex)
        return;
    const bool effects = !d.corners.empty() || d.cut.valid();
    int source = d.tex.target == GL_TEXTURE_EXTERNAL_OES ? 2 : d.tex.has_alpha ? 0 : 1;
    Program& p = r_.shaders().get(Shader::Tex, source + (effects ? 3 : 0));
    if (!p.id)
        return;

    if (d.wait_timeline) {
        int fd = wlr_drm_syncobj_timeline_export_sync_file(d.wait_timeline, d.wait_point);
        if (fd < 0)
            return;
        EGLSyncKHR sync = r_.egl().create_sync(fd);
        close(fd);
        if (sync == EGL_NO_SYNC_KHR)
            return;
        const bool ok = r_.egl().wait_sync(sync);
        r_.egl().destroy_sync(sync);
        if (!ok)
            return;
    }

    wlr_fbox src = d.src;
    if (src.width <= 0 || src.height <= 0)
        src = {0, 0, double(d.tex.width), double(d.tex.height)};
    src.x /= d.tex.width;
    src.y /= d.tex.height;
    src.width /= d.tex.width;
    src.height /= d.tex.height;

    const bool translucent = d.tex.has_alpha || d.alpha < 1 || effects;
    if (translucent && d.blend)
        glEnable(GL_BLEND);
    else
        glDisable(GL_BLEND);

    pixman_region32_t clip;
    if (d.clip) {
        pixman_region32_init(&clip);
        pixman_region32_copy(&clip, d.clip);
    } else {
        clip = region_of(d.dst);
    }
    cut_from_clip(&clip, d.cut);

    glUseProgram(p.id);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(d.tex.target, d.tex.tex);
    filter(d.tex.target, d.filter);
    p.set("tex", 0);
    p.set("alpha", d.alpha);
    p.set("discard_transparent", d.discard_transparent ? 1 : 0);

    // Content in another transfer function or gamut is converted to what
    // the frame is drawn in; plain SDR is drawn as is.
    int hdr_tf = tf_index(d.transfer);
    float prim[9];
    float lum = d.luminance;
    matrix::identity(prim);
    if (d.transfer == 0 && d.tex.encoded) {
        // A copy of an HDR screen's buffer (a screenshot): back to SDR.
        hdr_tf = d.tex.encoded->encoded_tf;
        std::memcpy(prim, d.tex.encoded->encoded_matrix, sizeof(prim));
        lum = 1;
    } else if (d.primaries) {
        wlr_color_primaries srgb;
        wlr_color_primaries_from_named(&srgb, WLR_COLOR_NAMED_PRIMARIES_SRGB);
        wlr_color_primaries_transform_absolute_colorimetric(d.primaries, &srgb, prim);
        if (hdr_tf == 0 && !matrix::is_identity(prim))
            hdr_tf = 3;  // SDR in another gamut: decoded as gamma 2.2
    }
    p.set("hdr_tf", hdr_tf);
    p.set_mat3("hdr_prim", prim);
    p.set("hdr_lum", lum);

    if (effects) {
        const FBox& rb = d.round_box.empty() ? d.dst : d.round_box;
        p.set("size", float(rb.width), float(rb.height));
        p.set("position", float(rb.x), float(rb.y));
        set_corners(p, "radius_top_left", "radius_top_right", "radius_bottom_left", "radius_bottom_right", d.corners);
        set_cut(p, d.cut);
    }
    set_proj(p, d.dst);
    set_tex_matrix(p, d.transform, src);
    draw(p, d.dst, &clip);
    pixman_region32_fini(&clip);
    glBindTexture(d.tex.target, 0);
}

void RenderPass::add_rect(const RectDraw& d) {
    if (d.box.empty())
        return;
    const bool cut = d.cut.valid();
    const bool round = !d.corners.empty();
    const bool blend = d.blend && (d.color[3] < 1 || cut || round);
    if (!blend && !d.clip && !round && d.box.x == 0 && d.box.y == 0 && d.box.width == width_ &&
        d.box.height == height_) {
        glClearColor(d.color[0], d.color[1], d.color[2], d.color[3]);
        glClear(GL_COLOR_BUFFER_BIT);
        return;
    }
    pixman_region32_t clip;
    if (d.clip) {
        pixman_region32_init(&clip);
        pixman_region32_copy(&clip, d.clip);
    } else {
        clip = region_of(d.box);
    }
    cut_from_clip(&clip, d.cut);
    if (blend)
        glEnable(GL_BLEND);
    else
        glDisable(GL_BLEND);

    Program& p = round ? r_.shaders().get(Shader::QuadRound) : r_.shaders().get(Shader::Quad, cut ? 1 : 0);
    glUseProgram(p.id);
    set_proj(p, d.box);
    p.set("color", d.color[0], d.color[1], d.color[2], d.color[3]);
    if (round) {
        p.set("size", float(d.box.width), float(d.box.height));
        p.set("position", float(d.box.x), float(d.box.y));
        set_corners(p, "radius_top_left", "radius_top_right", "radius_bottom_left", "radius_bottom_right", d.corners);
    }
    if (round || cut)
        set_cut(p, d.cut);
    draw(p, d.box, &clip);
    pixman_region32_fini(&clip);
}

void RenderPass::add_shadow(const ShadowDraw& d) {
    if (d.box.empty())
        return;
    pixman_region32_t clip;
    if (d.clip) {
        pixman_region32_init(&clip);
        pixman_region32_copy(&clip, d.clip);
    } else {
        clip = region_of(d.box);
    }
    cut_from_clip(&clip, d.cut);

    // The shader gives straight alpha.
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    Program& p = r_.shaders().get(Shader::BoxShadow);
    glUseProgram(p.id);
    set_proj(p, d.box);
    p.set("color", d.color[0], d.color[1], d.color[2], d.color[3]);
    p.set("blur_sigma", d.sigma);
    p.set("size", float(d.box.width), float(d.box.height));
    p.set("position", float(d.box.x), float(d.box.y));
    p.set("corner_radius", d.radius);
    set_cut(p, d.cut);
    draw(p, d.box, &clip);
    pixman_region32_fini(&clip);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
}

// ---- blur ---------------------------------------------------------------

Framebuffer* RenderPass::blur_into(const BlurParams& params, Framebuffer* source, const pixman_region32_t* region) {
    if (!fx_ || !params.enabled())
        return nullptr;
    const FBox full{0, 0, double(width_), double(height_)};
    const wlr_fbox whole{0, 0, 1, 1};

    pixman_region32_t damage;
    pixman_region32_init(&damage);
    if (region)
        pixman_region32_copy(&damage, region);
    else
        pixman_region32_union_rect(&damage, &damage, 0, 0, unsigned(width_), unsigned(height_));
    wlr_region_expand(&damage, &damage, params.reach());
    pixman_region32_intersect_rect(&damage, &damage, 0, 0, unsigned(width_), unsigned(height_));

    pixman_region32_t scaled;
    pixman_region32_init(&scaled);
    Framebuffer* current = source;
    // The screen's buffer is read through its framebuffer (see copy).
    if (source->buffer && blit(&damage, fx_->effects.get(), source))
        current = fx_->effects.get();
    glDisable(GL_BLEND);
    glDisable(GL_STENCIL_TEST);

    auto step = [&](Program& p, bool down) {
        Framebuffer* dst = current == fx_->effects.get() ? fx_->effects_swapped.get() : fx_->effects.get();
        bind(dst);
        const TexRef t = sampler(current);
        glUseProgram(p.id);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(t.target, t.tex);
        filter(t.target, WLR_SCALE_FILTER_BILINEAR);
        p.set("tex", 0);
        p.set("radius", params.radius);
        if (down)
            p.set("halfpixel", 0.5f / (width_ / 2.0f), 0.5f / (height_ / 2.0f));
        else
            p.set("halfpixel", 0.5f / (width_ * 2.0f), 0.5f / (height_ * 2.0f));
        set_proj(p, full);
        set_tex_matrix(p, WL_OUTPUT_TRANSFORM_NORMAL, whole);
        draw(p, full, &scaled);
        glBindTexture(t.target, 0);
        current = dst;
    };

    Program& down = r_.shaders().get(Shader::Blur1);
    Program& up = r_.shaders().get(Shader::Blur2);
    for (int i = 0; i < params.passes; ++i) {
        wlr_region_scale(&scaled, &damage, 1.0f / float(1 << (i + 1)));
        step(down, true);
    }
    for (int i = params.passes - 1; i >= 0; --i) {
        wlr_region_scale(&scaled, &damage, 1.0f / float(1 << i));
        step(up, false);
    }

    if (params.has_effects() && pixman_region32_not_empty(&damage)) {
        Program& p = r_.shaders().get(Shader::BlurEffects);
        Framebuffer* dst = current == fx_->effects.get() ? fx_->effects_swapped.get() : fx_->effects.get();
        bind(dst);
        const TexRef t = sampler(current);
        glUseProgram(p.id);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(t.target, t.tex);
        filter(t.target, WLR_SCALE_FILTER_BILINEAR);
        p.set("tex", 0);
        p.set("noise", params.noise);
        p.set("brightness", params.brightness);
        p.set("contrast", params.contrast);
        p.set("saturation", params.saturation);
        set_proj(p, full);
        set_tex_matrix(p, WL_OUTPUT_TRANSFORM_NORMAL, whole);
        draw(p, full, &damage);
        glBindTexture(t.target, 0);
        current = dst;
    }
    pixman_region32_fini(&scaled);
    pixman_region32_fini(&damage);
    bind(fb_);
    return current;
}

void RenderPass::add_blur(const BlurDraw& d) {
    if (!fx_ || !blur_params || d.box.empty())
        return;
    const bool weaker = d.strength < 1;
    Framebuffer* blurred = nullptr;
    if (d.use_cache && !weaker) {
        blurred = fx_->cache_blurred.get();
    } else {
        // Weaker than the cache: blur the cache's unblurred copy again.
        Framebuffer* source = d.use_cache ? fx_->cache_plain.get() : fb_;
        blurred = blur_into(blur_params->scaled(d.strength), source, d.clip);
    }
    if (!blurred)
        return;

    // Glass finds its shape in the mask itself (and draws its shadow
    // outside it); plain blur is stencilled to where the mask has pixels.
    const bool stencil = d.mask && d.refraction <= 0;
    if (stencil) {
        stencil_begin();
        TextureDraw m;
        m.tex = *d.mask;
        m.src = d.mask_src;
        m.dst = d.box;
        m.transform = d.mask_transform;
        m.clip = d.clip;
        m.discard_transparent = true;
        m.corners = d.corners;
        m.cut = d.cut;
        add_texture(m);
        stencil_inside();
    }

    if (d.refraction > 0) {
        render_glass(blurred, d);
    } else {
        TextureDraw t;
        t.tex = sampler(blurred);
        t.dst = {0, 0, double(width_), double(height_)};
        t.clip = d.clip;
        t.alpha = d.alpha;
        t.corners = d.corners;
        t.round_box = d.box;
        t.cut = d.cut;
        add_texture(t);
    }
    if (stencil)
        stencil_end();
}

bool RenderPass::render_blur_cache(const FBox& box) {
    if (!fx_ || !blur_params)
        return false;
    pixman_region32_t clip = region_of(box);
    Framebuffer* blurred = blur_into(*blur_params, fb_, &clip);
    if (blurred) {
        copy(&clip, fx_->cache_blurred.get(), blurred);
        copy(&clip, fx_->cache_plain.get(), fb_);
    }
    pixman_region32_fini(&clip);
    return blurred != nullptr;
}

bool RenderPass::blit(const pixman_region32_t* region, Framebuffer* dst, Framebuffer* src) {
    const GLuint read = src->get_fbo(), write = dst->get_fbo();
    if (!read || !write || src->width != dst->width || src->height != dst->height)
        return false;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, read);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, write);
    glDisable(GL_SCISSOR_TEST);
    int n = 0;
    const pixman_box32_t* rects = pixman_region32_rectangles(region, &n);
    for (int i = 0; i < n; ++i)
        glBlitFramebuffer(rects[i].x1, rects[i].y1, rects[i].x2, rects[i].y2, rects[i].x1, rects[i].y1,
                          rects[i].x2, rects[i].y2, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    bind(fb_);
    return true;
}

void RenderPass::copy(const pixman_region32_t* region, Framebuffer* dst, Framebuffer* src) {
    if (!region || !pixman_region32_not_empty(region) || !dst || !src)
        return;
    // A buffer we draw into (the screen's) is read through its framebuffer,
    // which always has what was just drawn.
    if (src->buffer && blit(region, dst, src))
        return;
    bind(dst);
    Framebuffer* keep = fb_;
    TextureDraw t;
    t.tex = sampler(src);
    t.tex.encoded = nullptr;  // a copy, not a conversion
    t.dst = {0, 0, double(dst->width), double(dst->height)};
    t.clip = region;
    t.blend = false;
    add_texture(t);
    fb_ = keep;
    bind(fb_);
}

// ---- Liquid Glass -------------------------------------------------------

Framebuffer* RenderPass::glass_field(const BlurDraw& d, const TexRef* mask, const float mask_norm[4], float sigma) {
    if (!fx_->field.ensure(r_, width_, height_, GL_RGBA16F) ||
        !fx_->field_swapped.ensure(r_, width_, height_, GL_RGBA16F))
        return nullptr;
    Program& p = r_.shaders().get(Shader::GlassField);
    const FBox full{0, 0, double(width_), double(height_)};
    const wlr_fbox whole{0, 0, 1, 1};
    const int reach = int(std::ceil(3 * sigma)) + 2;
    const FBox& mb = mask ? d.mask_box : d.box;
    const int nx = int(std::floor(d.box.x)), ny = int(std::floor(d.box.y));
    const int nw = int(std::ceil(d.box.x + d.box.width)) - nx, nh = int(std::ceil(d.box.y + d.box.height)) - ny;

    glDisable(GL_BLEND);
    glUseProgram(p.id);
    set_proj(p, full);
    set_tex_matrix(p, WL_OUTPUT_TRANSFORM_NORMAL, whole);
    if (mask) {
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, mask->tex);
        filter(GL_TEXTURE_2D, WLR_SCALE_FILTER_BILINEAR);
        p.set("mask", 1);
        p.set("mask_src", mask_norm[0], mask_norm[1], mask_norm[2], mask_norm[3]);
        glActiveTexture(GL_TEXTURE0);
    }
    p.set("has_mask", mask ? 1 : 0);
    p.set("box_pos", float(mb.x), float(mb.y));
    p.set("box_size", float(mb.width), float(mb.height));
    p.set("radius", d.corners.tl);
    p.set("sigma", sigma);
    p.set("texel", 1.0f / width_, 1.0f / height_);
    p.set("tex", 0);

    // Across, over rows reaching past the node by the kernel, which the
    // second pass reads.
    pixman_region32_t region;
    pixman_region32_init_rect(&region, nx, ny - reach, unsigned(nw), unsigned(nh + 2 * reach));
    bind(fx_->field_swapped.get());
    p.set("first", 1);
    p.set("dir", 1.0f, 0.0f);
    draw(p, full, &region);
    pixman_region32_fini(&region);

    // Down, over the node (and a pixel round it, for glass.frag's gradient).
    pixman_region32_init_rect(&region, nx - 1, ny - 1, unsigned(nw + 2), unsigned(nh + 2));
    bind(fx_->field.get());
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, fx_->field_swapped->tex);
    filter(GL_TEXTURE_2D, WLR_SCALE_FILTER_NEAREST);
    p.set("first", 0);
    p.set("dir", 0.0f, 1.0f);
    draw(p, full, &region);
    pixman_region32_fini(&region);
    glBindTexture(GL_TEXTURE_2D, 0);
    if (mask) {
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, 0);
        glActiveTexture(GL_TEXTURE0);
    }
    bind(fb_);
    return fx_->field.get();
}

void RenderPass::render_glass(Framebuffer* blurred, const BlurDraw& d) {
    const TexRef* mask = d.mask && d.mask->target == GL_TEXTURE_2D ? d.mask : nullptr;
    float mask_norm[4] = {0, 0, 1, 1};
    if (mask && d.mask_src.width > 0 && d.mask_src.height > 0) {
        mask_norm[0] = float(d.mask_src.x / mask->width);
        mask_norm[1] = float(d.mask_src.y / mask->height);
        mask_norm[2] = float(d.mask_src.width / mask->width);
        mask_norm[3] = float(d.mask_src.height / mask->height);
    }
    const FBox& mb = mask ? d.mask_box : d.box;
    const float sigma = std::max(d.thickness / 2.5f, 1.0f);
    // Shapes given: their exact geometry. Otherwise, the mask's, blurred.
    Framebuffer* field = d.shape_count > 0 ? nullptr : glass_field(d, mask, mask_norm, sigma);

    glEnable(GL_BLEND);
    pixman_region32_t clip;
    if (d.clip) {
        pixman_region32_init(&clip);
        pixman_region32_copy(&clip, d.clip);
    } else {
        pixman_region32_init_rect(&clip, 0, 0, unsigned(width_), unsigned(height_));
    }

    Program& p = r_.shaders().get(Shader::Glass);
    glUseProgram(p.id);
    const TexRef b = sampler(blurred);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(b.target, b.tex);
    filter(b.target, WLR_SCALE_FILTER_BILINEAR);
    p.set("tex", 0);
    if (mask) {
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, mask->tex);
        // Linear, so the edge is found between pixels, not on them.
        filter(GL_TEXTURE_2D, WLR_SCALE_FILTER_BILINEAR);
        p.set("mask", 1);
        p.set("mask_src", mask_norm[0], mask_norm[1], mask_norm[2], mask_norm[3]);
    }
    p.set("has_mask", mask ? 1 : 0);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, field ? field->tex : 0);
    filter(GL_TEXTURE_2D, WLR_SCALE_FILTER_BILINEAR);
    p.set("field", 2);
    p.set("field_texel", 1.0f / width_, 1.0f / height_);
    p.set("field_sigma", sigma);
    glActiveTexture(GL_TEXTURE0);

    const int count = std::min(d.shape_count, 16);
    if (count > 0) {
        GLfloat boxes[16 * 4], extras[16 * 2], clips[16 * 4];
        for (int i = 0; i < count; ++i) {
            const GlassShapeDraw& s = d.shapes[i];
            boxes[i * 4 + 0] = s.x;
            boxes[i * 4 + 1] = s.y;
            boxes[i * 4 + 2] = s.width;
            boxes[i * 4 + 3] = s.height;
            extras[i * 2 + 0] = s.radius;
            extras[i * 2 + 1] = s.opacity;
            clips[i * 4 + 0] = s.clip_x;
            clips[i * 4 + 1] = s.clip_y;
            clips[i * 4 + 2] = s.clip_width;
            clips[i * 4 + 3] = s.clip_height;
        }
        glUniform4fv(p.loc("shape_box"), count, boxes);
        glUniform2fv(p.loc("shape_extra"), count, extras);
        glUniform4fv(p.loc("shape_clip"), count, clips);
    }
    p.set("shape_count", count);
    p.set("texel", 1.0f / width_, 1.0f / height_);
    p.set("box_pos", float(mb.x), float(mb.y));
    p.set("box_size", float(mb.width), float(mb.height));
    p.set("radius", d.corners.tl);
    p.set("refraction", d.refraction);
    p.set("thickness", d.thickness);
    p.set("alpha", d.alpha);
    const GlassMaterial& g = d.glass;
    p.set("tint", g.tint[0], g.tint[1], g.tint[2], g.tint[3]);
    p.set("adapt", g.adapt);
    p.set("saturation", g.saturation);
    p.set("highlight", g.highlight);
    p.set("light_dir", g.light_dir[0], g.light_dir[1]);
    p.set("shadow", g.shadow);

    const FBox full{0, 0, double(width_), double(height_)};
    set_proj(p, full);
    set_tex_matrix(p, WL_OUTPUT_TRANSFORM_NORMAL, wlr_fbox{0, 0, 1, 1});
    draw(p, full, &clip);
    pixman_region32_fini(&clip);

    if (mask) {
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, 0);
    }
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(b.target, 0);
}

// ---- colour output --------------------------------------------------------

void RenderPass::output_pass() {
    Framebuffer* blend = fb_;
    bind(output_fb_);
    glDisable(GL_BLEND);
    glDisable(GL_STENCIL_TEST);
    Program& p = r_.shaders().get(Shader::Output);
    glUseProgram(p.id);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, blend->tex);
    filter(GL_TEXTURE_2D, WLR_SCALE_FILTER_NEAREST);
    p.set("tex", 0);
    p.set_mat3("matrix", color_.matrix);
    p.set("out_tf", color_.tf);
    ColorLut* lut = color_.lut;
    if (lut && !lut->tex && lut->size > 1) {
        glGenTextures(1, &lut->tex);
        glBindTexture(GL_TEXTURE_3D, lut->tex);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        for (GLenum w : {GL_TEXTURE_WRAP_S, GL_TEXTURE_WRAP_T, GL_TEXTURE_WRAP_R})
            glTexParameteri(GL_TEXTURE_3D, w, GL_CLAMP_TO_EDGE);
        glTexImage3D(GL_TEXTURE_3D, 0, GL_RGB16F, lut->size, lut->size, lut->size, 0, GL_RGB, GL_FLOAT,
                     lut->rgb.data());
    }
    const bool use_lut = lut && lut->tex;
    p.set("has_lut", use_lut ? 1 : 0);
    if (use_lut) {
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_3D, lut->tex);
        p.set("lut", 1);
        p.set("lut_size", float(lut->size));
        glActiveTexture(GL_TEXTURE0);
    }
    const FBox full{0, 0, double(width_), double(height_)};
    set_proj(p, full);
    set_tex_matrix(p, WL_OUTPUT_TRANSFORM_NORMAL, wlr_fbox{0, 0, 1, 1});
    draw(p, full, nullptr);
    glBindTexture(GL_TEXTURE_2D, 0);
    if (use_lut) {
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_3D, 0);
        glActiveTexture(GL_TEXTURE0);
    }
    glEnable(GL_BLEND);

    // Say what these pixels are, so copies (screenshots, screen sharing)
    // can turn them back into what SDR content looks like.
    output_fb_->encoded_tf = 0;
    if (color_.tf == 1 || color_.tf == 2) {
        if (matrix::invert(output_fb_->encoded_matrix, color_.matrix))
            output_fb_->encoded_tf = color_.tf;
    }
    fb_ = output_fb_;
    output_fb_ = nullptr;
}

bool RenderPass::push_target(Target& t, int w, int h) {
    if (w <= 0 || h <= 0 || !t.ensure(r_, w, h, two_pass_ ? GL_RGBA16F : GL_RGBA8))
        return false;
    Saved was{fb_, width_, height_, {}};
    std::memcpy(was.proj, proj_, sizeof(proj_));
    saved_.push_back(was);
    fb_ = t.get();
    width_ = w;
    height_ = h;
    matrix::projection(proj_, w, h, WL_OUTPUT_TRANSFORM_FLIPPED_180);
    bind(fb_);
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    return true;
}

void RenderPass::pop_target() {
    if (saved_.empty())
        return;
    const Saved was = saved_.back();
    saved_.pop_back();
    fb_ = was.fb;
    width_ = was.width;
    height_ = was.height;
    std::memcpy(proj_, was.proj, sizeof(proj_));
    bind(fb_);
}

void RenderPass::apply_screen_shader(const pixman_region32_t* region) {
    Program& p = r_.shaders().get(Shader::Screen);
    if (!p.id || !fx_ || !region || !pixman_region32_not_empty(region))
        return;
    copy(region, fx_->effects.get(), fb_);
    glDisable(GL_BLEND);
    glUseProgram(p.id);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, fx_->effects->tex);
    filter(GL_TEXTURE_2D, WLR_SCALE_FILTER_NEAREST);
    p.set("tex", 0);
    timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    p.set("time", float(now.tv_sec % 100000) + now.tv_nsec / 1e9f);
    const FBox full{0, 0, double(width_), double(height_)};
    set_proj(p, full);
    set_tex_matrix(p, WL_OUTPUT_TRANSFORM_NORMAL, wlr_fbox{0, 0, 1, 1});
    draw(p, full, region);
    glBindTexture(GL_TEXTURE_2D, 0);
    glEnable(GL_BLEND);
}

} // namespace atrium::render
