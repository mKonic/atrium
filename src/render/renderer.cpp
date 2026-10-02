#include "render/renderer.hpp"
#include "util/log.hpp"

#include "backend/backend.hpp"

#include "render/matrix.hpp"
#include "render/pass.hpp"
#include "render/shaders.hpp"

#include <drm_fourcc.h>
#include <fcntl.h>
#include <unistd.h>
#include <xf86drm.h>

#include <cstdlib>
#include <cstring>
#include <ctime>
#include <vector>

namespace atrium::render {

bool OutputColor::plain() const {
    return matrix::is_identity(matrix) && (tf == 0 || tf == 3) && !lut;
}

namespace {

int64_t ns(const timespec& t) { return int64_t(t.tv_sec) * 1000000000 + t.tv_nsec; }

const char* reset_status(GLenum status) {
    switch (status) {
    case GL_GUILTY_CONTEXT_RESET_KHR:
        return "guilty";
    case GL_INNOCENT_CONTEXT_RESET_KHR:
        return "innocent";
    case GL_UNKNOWN_CONTEXT_RESET_KHR:
        return "unknown";
    default:
        return "?";
    }
}

void gl_log(GLenum, GLenum type, GLuint, GLenum, GLsizei, const GLchar* msg, const void*) {
    const bool bad = type == GL_DEBUG_TYPE_ERROR_KHR || type == GL_DEBUG_TYPE_UNDEFINED_BEHAVIOR_KHR;
    alog(bad ? Log::Error : Log::Debug, "[GLES] %s", msg);
}

// Any render node, for backends that take dmabufs without naming a GPU.
int open_any_render_node() {
    int n = drmGetDevices2(0, nullptr, 0);
    if (n <= 0)
        return -1;
    std::vector<drmDevice*> devices(n);
    n = drmGetDevices2(0, devices.data(), n);
    int fd = -1;
    for (int i = 0; i < n && fd < 0; ++i)
        if (devices[i]->available_nodes & (1 << DRM_NODE_RENDER))
            fd = open(devices[i]->nodes[DRM_NODE_RENDER], O_RDWR | O_CLOEXEC);
    for (int i = 0; i < n; ++i)
        drmFreeDevice(&devices[i]);
    return fd;
}

void framebuffer_addon_destroy(wlr_addon* addon) {
    Framebuffer* fb = wl_container_of(addon, fb, addon);
    delete fb;
}

const wlr_addon_interface kFramebufferAddon = {
    .name = "atrium_framebuffer",
    .destroy = framebuffer_addon_destroy,
};

} // namespace

// ---- Framebuffer ------------------------------------------------------

GLuint Framebuffer::get_fbo() {
    if (fbo)
        return fbo;
    if (external_only || !buffer)
        return 0;
    if (!renderer->procs.glEGLImageTargetRenderbufferStorageOES)
        return 0;
    if (!rbo) {
        glGenRenderbuffers(1, &rbo);
        glBindRenderbuffer(GL_RENDERBUFFER, rbo);
        renderer->procs.glEGLImageTargetRenderbufferStorageOES(GL_RENDERBUFFER, image);
        glBindRenderbuffer(GL_RENDERBUFFER, 0);
    }
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rbo);
    // A stencil for blur masks.
    glGenRenderbuffers(1, &stencil);
    glBindRenderbuffer(GL_RENDERBUFFER, stencil);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_STENCIL_INDEX8, width, height);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, stencil);
    const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        alog(Log::Error, "Framebuffer incomplete (0x%x)", status);
        glDeleteFramebuffers(1, &fbo);
        fbo = 0;
    }
    return fbo;
}

void Framebuffer::bind() {
    glBindFramebuffer(GL_FRAMEBUFFER, get_fbo());
}

TexRef Framebuffer::texture() const {
    return {GL_TEXTURE_2D, tex, width, height, true, encoded_tf ? this : nullptr};
}

Framebuffer::~Framebuffer() {
    renderer->framebuffers.erase(this);
    if (buffer)
        wlr_addon_finish(&addon);
    renderer->egl().make_current();
    if (fbo)
        glDeleteFramebuffers(1, &fbo);
    if (rbo)
        glDeleteRenderbuffers(1, &rbo);
    if (tex)
        glDeleteTextures(1, &tex);
    if (stencil)
        glDeleteRenderbuffers(1, &stencil);
    renderer->egl().destroy_image(image);
}

// ---- Target -----------------------------------------------------------

Target::~Target() { release(); }

void Target::release() { fb_.reset(); }

bool Target::ensure(Renderer& r, int width, int height, GLenum internal_format) {
    if (fb_ && fb_->width == width && fb_->height == height && fb_->internal_format == internal_format)
        return true;
    fb_.reset();
    auto fb = std::make_unique<Framebuffer>();
    fb->renderer = &r;
    fb->width = width;
    fb->height = height;
    fb->internal_format = internal_format;
    r.framebuffers.insert(fb.get());

    GLenum type = GL_UNSIGNED_BYTE;
    if (internal_format == GL_RGBA16F)
        type = GL_HALF_FLOAT;
    else if (internal_format == GL_RGB10_A2)
        type = GL_UNSIGNED_INT_2_10_10_10_REV;
    glGenTextures(1, &fb->tex);
    glBindTexture(GL_TEXTURE_2D, fb->tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, internal_format, width, height, 0, GL_RGBA, type, nullptr);
    glBindTexture(GL_TEXTURE_2D, 0);

    glGenFramebuffers(1, &fb->fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fb->fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, fb->tex, 0);
    glGenRenderbuffers(1, &fb->stencil);
    glBindRenderbuffer(GL_RENDERBUFFER, fb->stencil);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_STENCIL_INDEX8, width, height);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, fb->stencil);
    const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        alog(Log::Error, "Offscreen target 0x%x %dx%d incomplete (0x%x)", internal_format, width, height, status);
        return false;
    }
    fb_ = std::move(fb);
    return true;
}

// ---- Texture ----------------------------------------------------------

TexRef Texture::ref() const {
    const Framebuffer* enc = buffer && buffer->encoded_tf ? buffer : nullptr;
    return {target, tex, int(base.width), int(base.height), has_alpha, enc};
}

namespace {

Texture* tex_of(wlr_texture* t) { return reinterpret_cast<Texture*>(t); }

void texture_destroy(Texture* t) {
    Renderer* r = t->renderer;
    r->textures.erase(t);
    if (t->buffer) {
        wlr_buffer_unlock(t->buffer->buffer);
    } else {
        r->egl().make_current();
        if (t->tex)
            glDeleteTextures(1, &t->tex);
    }
    if (t->fbo) {
        r->egl().make_current();
        glDeleteFramebuffers(1, &t->fbo);
    }
    delete t;
}

bool texture_update(wlr_texture* wt, wlr_buffer* buffer, const pixman_region32_t* damage) {
    Texture* t = tex_of(wt);
    if (!t->drm_format)
        return false;
    void* data;
    uint32_t format;
    size_t stride;
    if (!wlr_buffer_begin_data_ptr_access(buffer, WLR_BUFFER_DATA_PTR_ACCESS_READ, &data, &format, &stride))
        return false;
    const PixelFormat* f = format_from_drm(t->drm_format);
    if (format != t->drm_format || !f || stride % f->bytes_per_pixel ||
        stride < size_t(buffer->width) * f->bytes_per_pixel) {
        wlr_buffer_end_data_ptr_access(buffer);
        return false;
    }
    t->renderer->egl().make_current();
    glBindTexture(GL_TEXTURE_2D, t->tex);
    int n = 0;
    const pixman_box32_t* rects = pixman_region32_rectangles(damage, &n);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, GLint(stride / f->bytes_per_pixel));
    for (int i = 0; i < n; ++i) {
        glPixelStorei(GL_UNPACK_SKIP_PIXELS, rects[i].x1);
        glPixelStorei(GL_UNPACK_SKIP_ROWS, rects[i].y1);
        glTexSubImage2D(GL_TEXTURE_2D, 0, rects[i].x1, rects[i].y1, rects[i].x2 - rects[i].x1,
                        rects[i].y2 - rects[i].y1, f->gl_format, f->gl_type, data);
    }
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
    wlr_buffer_end_data_ptr_access(buffer);
    return true;
}

// The framebuffer to read a texture's pixels from.
bool texture_bind_for_read(Texture* t) {
    if (t->fbo) {
        glBindFramebuffer(GL_FRAMEBUFFER, t->fbo);
        return true;
    }
    if (t->buffer) {
        GLuint fbo = t->buffer->get_fbo();
        if (!fbo)
            return false;
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        return true;
    }
    glGenFramebuffers(1, &t->fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, t->fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, t->target, t->tex, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        glDeleteFramebuffers(1, &t->fbo);
        t->fbo = 0;
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        return false;
    }
    return true;
}

// Packs one RGBA-ordered row (R lowest) into `format`.
void pack_row(uint32_t format, const uint32_t* row, int width, unsigned char* out) {
    for (int x = 0; x < width; ++x) {
        const uint32_t v = row[x];
        switch (format) {
        case DRM_FORMAT_XBGR8888:
        case DRM_FORMAT_ABGR8888:
        case DRM_FORMAT_XBGR2101010:
        case DRM_FORMAT_ABGR2101010:
            reinterpret_cast<uint32_t*>(out)[x] = v;
            break;
        case DRM_FORMAT_XRGB8888:
        case DRM_FORMAT_ARGB8888:
            reinterpret_cast<uint32_t*>(out)[x] = (v & 0xFF00FF00u) | ((v & 0xFFu) << 16) | ((v >> 16) & 0xFFu);
            break;
        case DRM_FORMAT_XRGB2101010:
        case DRM_FORMAT_ARGB2101010:
            reinterpret_cast<uint32_t*>(out)[x] =
                (v & 0xC00FFC00u) | ((v & 0x3FFu) << 20) | ((v >> 20) & 0x3FFu);
            break;
        case DRM_FORMAT_BGR888:  // memory: R, G, B
            out[x * 3] = v & 0xFF;
            out[x * 3 + 1] = (v >> 8) & 0xFF;
            out[x * 3 + 2] = (v >> 16) & 0xFF;
            break;
        case DRM_FORMAT_RGB888:  // memory: B, G, R
            out[x * 3] = (v >> 16) & 0xFF;
            out[x * 3 + 1] = (v >> 8) & 0xFF;
            out[x * 3 + 2] = v & 0xFF;
            break;
        }
    }
}

// An HDR screen's pixels read as SDR (screenshots): drawn, converted, into
// a temporary target first, then read from there.
bool read_hdr_as_sdr(Texture* t, const wlr_texture_read_pixels_options* o, bool deep) {
    Renderer& r = *t->renderer;
    r.egl().make_current();
    Target tmp;
    if (!tmp.ensure(r, int(t->base.width), int(t->base.height), deep ? GL_RGB10_A2 : GL_RGBA8))
        return false;
    RenderPass* pass = r.begin(tmp.get(), {});
    if (!pass)
        return false;
    TextureDraw d;
    d.tex = t->ref();
    d.dst = {0, 0, double(t->base.width), double(t->base.height)};
    d.blend = false;
    pass->add_texture(d);
    if (!pass->submit())
        return false;

    wlr_box src;
    wlr_texture_read_pixels_options_get_src_box(o, &t->base, &src);
    auto* p = static_cast<unsigned char*>(wlr_texture_read_pixel_options_get_data(o));
    std::vector<uint32_t> row(src.width);
    r.egl().make_current();
    glBindFramebuffer(GL_FRAMEBUFFER, tmp->fbo);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glGetError();
    for (int i = 0; i < src.height; ++i) {
        glReadPixels(src.x, src.y + i, src.width, 1, GL_RGBA,
                     deep ? GL_UNSIGNED_INT_2_10_10_10_REV : GL_UNSIGNED_BYTE, row.data());
        pack_row(o->format, row.data(), src.width, p + size_t(i) * o->stride);
    }
    const bool ok = glGetError() == GL_NO_ERROR;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return ok;
}

bool texture_read_pixels(wlr_texture* wt, const wlr_texture_read_pixels_options* o) {
    Texture* t = tex_of(wt);
    Renderer& r = *t->renderer;
    if (t->buffer && t->buffer->encoded_tf) {
        switch (o->format) {
        case DRM_FORMAT_XRGB2101010:
        case DRM_FORMAT_ARGB2101010:
        case DRM_FORMAT_XBGR2101010:
        case DRM_FORMAT_ABGR2101010:
            return read_hdr_as_sdr(t, o, true);
        case DRM_FORMAT_XRGB8888:
        case DRM_FORMAT_ARGB8888:
        case DRM_FORMAT_XBGR8888:
        case DRM_FORMAT_ABGR8888:
        case DRM_FORMAT_BGR888:
        case DRM_FORMAT_RGB888:
            return read_hdr_as_sdr(t, o, false);
        default:
            alog(Log::Error, "Can't read an HDR buffer as 0x%08x", o->format);
            return false;
        }
    }

    const PixelFormat* f = format_from_drm(o->format);
    if (!f || !format_supported(r.caps(), *f)) {
        alog(Log::Error, "Can't read pixels as 0x%08x", o->format);
        return false;
    }
    if (f->gl_format == GL_BGRA_EXT && !r.caps().EXT_read_format_bgra)
        return false;

    wlr_box src;
    wlr_texture_read_pixels_options_get_src_box(o, wt, &src);
    if (!r.egl().make_current() || !texture_bind_for_read(t))
        return false;
    glFinish();
    glGetError();
    auto* p = static_cast<unsigned char*>(wlr_texture_read_pixel_options_get_data(o));
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    const uint32_t packed = uint32_t(src.width) * f->bytes_per_pixel;
    if (packed == o->stride && o->dst_x == 0) {
        glReadPixels(src.x, src.y, src.width, src.height, f->gl_format, f->gl_type, p);
    } else {
        for (int i = 0; i < src.height; ++i)
            glReadPixels(src.x, src.y + i, src.width, 1, f->gl_format, f->gl_type, p + size_t(i) * o->stride);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return glGetError() == GL_NO_ERROR;
}

uint32_t texture_preferred_read_format(wlr_texture* wt) {
    Texture* t = tex_of(wt);
    Renderer& r = *t->renderer;
    if (!r.egl().make_current() || !texture_bind_for_read(t))
        return DRM_FORMAT_INVALID;
    GLint format = -1, type = -1;
    glGetIntegerv(GL_IMPLEMENTATION_COLOR_READ_FORMAT, &format);
    glGetIntegerv(GL_IMPLEMENTATION_COLOR_READ_TYPE, &type);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (const PixelFormat* f = format_from_gl(format, type, t->has_alpha))
        return f->drm;
    return r.caps().EXT_read_format_bgra ? DRM_FORMAT_XRGB8888 : DRM_FORMAT_INVALID;
}

void texture_impl_destroy(wlr_texture* wt) { texture_destroy(tex_of(wt)); }

const wlr_texture_impl kTextureImpl = {
    .update_from_buffer = texture_update,
    .read_pixels = texture_read_pixels,
    .preferred_read_format = texture_preferred_read_format,
    .destroy = texture_impl_destroy,
};

} // namespace

// ---- wlr_renderer glue -------------------------------------------------

struct RendererImpl {
    static Renderer* self(wlr_renderer* r) { return reinterpret_cast<Renderer::Hook*>(r)->self; }

    static const wlr_drm_format_set* texture_formats(wlr_renderer* wr, uint32_t caps) {
        Renderer* r = self(wr);
        if (caps & WLR_BUFFER_CAP_DMABUF)
            return r->egl_->texture_formats();
        if (caps & WLR_BUFFER_CAP_DATA_PTR)
            return &r->shm_formats_;
        return nullptr;
    }

    static const wlr_drm_format_set* render_formats(wlr_renderer* wr) {
        return self(wr)->egl_->render_formats();
    }

    static int drm_fd(wlr_renderer* wr) {
        Renderer* r = self(wr);
        if (r->drm_fd_ < 0)
            r->drm_fd_ = r->egl_->dup_drm_fd();
        return r->drm_fd_;
    }

    static wlr_texture* texture_from_buffer(wlr_renderer* wr, wlr_buffer* b) {
        return self(wr)->texture_from_buffer(b);
    }

    static wlr_render_pass* begin_buffer_pass(wlr_renderer* wr, wlr_buffer* b, const wlr_buffer_pass_options* o) {
        Renderer* r = self(wr);
        if (!r->egl_->make_current())
            return nullptr;
        Framebuffer* fb = r->framebuffer_for(b);
        if (!fb)
            return nullptr;
        PassOptions po;
        po.timer = o->timer;
        po.signal_timeline = o->signal_timeline;
        po.signal_point = o->signal_point;
        if (o->color_transform) {
            // wlroots' transforms are opaque to us; atrium's own scene
            // hands its colour work over as PassOptions::color.
            static bool logged = false;
            if (!logged) {
                alog(Log::Info, "renderer: output color transforms from wlroots are not applied");
                logged = true;
            }
        }
        RenderPass* pass = r->begin(fb, po);
        return pass ? pass->wlr() : nullptr;
    }

    static wlr_render_timer* timer_create(wlr_renderer* wr) {
        Renderer* r = self(wr);
        if (!r->caps_.EXT_disjoint_timer_query)
            return nullptr;
        auto* t = new RenderTimer();
        t->base.impl = &timer_impl;
        t->renderer = r;
        r->egl_->make_current();
        r->procs.glGenQueriesEXT(1, &t->query);
        return &t->base;
    }

    static int timer_duration(wlr_render_timer* wt) {
        auto* t = reinterpret_cast<RenderTimer*>(wt);
        Renderer* r = t->renderer;
        r->egl_->make_current();
        GLint64 disjoint = 0;
        r->procs.glGetInteger64vEXT(GL_GPU_DISJOINT_EXT, &disjoint);
        if (disjoint)
            return -1;
        GLint available = 0;
        r->procs.glGetQueryObjectivEXT(t->query, GL_QUERY_RESULT_AVAILABLE_EXT, &available);
        if (!available)
            return -1;
        GLuint64 gl_end = 0;
        r->procs.glGetQueryObjectui64vEXT(t->query, GL_QUERY_RESULT_EXT, &gl_end);
        return int(int64_t(gl_end) - t->gl_cpu_end + ns(t->cpu_end) - ns(t->cpu_start));
    }

    static void timer_destroy(wlr_render_timer* wt) {
        auto* t = reinterpret_cast<RenderTimer*>(wt);
        t->renderer->egl_->make_current();
        t->renderer->procs.glDeleteQueriesEXT(1, &t->query);
        delete t;
    }

    static void destroy(wlr_renderer* wr) { delete self(wr); }

    static constexpr wlr_render_timer_impl timer_impl = {
        .get_duration_ns = timer_duration,
        .destroy = timer_destroy,
    };
    static constexpr wlr_renderer_impl impl = {
        .get_texture_formats = texture_formats,
        .get_render_formats = render_formats,
        .destroy = destroy,
        .get_drm_fd = drm_fd,
        .texture_from_buffer = texture_from_buffer,
        .begin_buffer_pass = begin_buffer_pass,
        .render_timer_create = timer_create,
    };
};

// ---- Renderer ----------------------------------------------------------

Renderer* Renderer::create(const backend::Backend& backend) {
    int drm_fd = -1;
    bool own_fd = false;
    if (const char* name = std::getenv("WLR_RENDER_DRM_DEVICE")) {
        drm_fd = open(name, O_RDWR | O_CLOEXEC);
        own_fd = drm_fd >= 0;
        if (drm_fd < 0)
            alog_errno(Log::Error, "Couldn't open %s", name);
    }
    if (drm_fd < 0)
        drm_fd = backend.drm_fd();
    if (drm_fd < 0 && (backend.buffer_caps() & WLR_BUFFER_CAP_DMABUF)) {
        drm_fd = open_any_render_node();
        own_fd = drm_fd >= 0;
    }
    if (drm_fd < 0) {
        alog(Log::Error, "No GPU to render with");
        return nullptr;
    }
    Renderer* r = create_on(drm_fd);
    if (own_fd)
        close(drm_fd);
    return r;
}

Renderer* Renderer::create_on(int drm_fd, bool software_ok) {
    auto egl = Egl::create(drm_fd, software_ok);
    if (!egl)
        return nullptr;

    auto* r = new Renderer();
    r->egl_ = std::move(egl);
    r->hook_.self = r;
    wlr_renderer_init(&r->hook_.base, &RendererImpl::impl, WLR_BUFFER_CAP_DMABUF);
    if (!r->init()) {
        wlr_renderer_destroy(&r->hook_.base);
        return nullptr;
    }
    return r;
}

Renderer* Renderer::from(wlr_renderer* r) {
    return r && r->WLR_PRIVATE.impl == &RendererImpl::impl ? RendererImpl::self(r) : nullptr;
}

Texture* Renderer::texture(wlr_texture* t) {
    return t && t->impl == &kTextureImpl ? tex_of(t) : nullptr;
}

bool Renderer::init() {
    if (!egl_->make_current())
        return false;
    const char* exts = reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
    if (!exts)
        return false;
    alog(Log::Info, "atrium renderer on %s (%s, %s)", glGetString(GL_VERSION), glGetString(GL_VENDOR),
            glGetString(GL_RENDERER));
    alog(Log::Debug, "GL extensions: %s", exts);

    if (!egl_->exts.EXT_image_dma_buf_import) {
        alog(Log::Error, "EGL_EXT_image_dma_buf_import unsupported");
        return false;
    }
    if (!has_extension(exts, "GL_EXT_texture_format_BGRA8888")) {
        alog(Log::Error, "GL_EXT_texture_format_BGRA8888 unsupported");
        return false;
    }
    caps_.EXT_read_format_bgra = has_extension(exts, "GL_EXT_read_format_bgra");
    caps_.EXT_texture_type_2_10_10_10_REV = has_extension(exts, "GL_EXT_texture_type_2_10_10_10_REV");
    caps_.OES_texture_half_float_linear = has_extension(exts, "GL_OES_texture_half_float_linear");
    caps_.EXT_texture_norm16 = has_extension(exts, "GL_EXT_texture_norm16");
    caps_.EXT_color_buffer_half_float = has_extension(exts, "GL_EXT_color_buffer_half_float") ||
                                        has_extension(exts, "GL_EXT_color_buffer_float");
    caps_.OES_egl_image_external_essl3 = has_extension(exts, "GL_OES_EGL_image_external_essl3");
    if (has_extension(exts, "GL_OES_EGL_image_external"))
        caps_.OES_egl_image_external = load_proc(procs.glEGLImageTargetTexture2DOES, "glEGLImageTargetTexture2DOES");
    if (has_extension(exts, "GL_OES_EGL_image"))
        caps_.OES_egl_image = load_proc(procs.glEGLImageTargetRenderbufferStorageOES,
                                        "glEGLImageTargetRenderbufferStorageOES") &&
                              load_proc(procs.glEGLImageTargetTexture2DOES, "glEGLImageTargetTexture2DOES");
    if (has_extension(exts, "GL_KHR_debug")) {
        caps_.KHR_debug = load_proc(procs.glDebugMessageCallbackKHR, "glDebugMessageCallbackKHR") &&
                          load_proc(procs.glDebugMessageControlKHR, "glDebugMessageControlKHR");
    }
    if (has_extension(exts, "GL_KHR_robustness")) {
        GLint strategy = 0;
        glGetIntegerv(GL_RESET_NOTIFICATION_STRATEGY_KHR, &strategy);
        if (strategy == GL_LOSE_CONTEXT_ON_RESET_KHR)
            caps_.KHR_robustness = load_proc(procs.glGetGraphicsResetStatusKHR, "glGetGraphicsResetStatusKHR");
    }
    if (has_extension(exts, "GL_EXT_disjoint_timer_query")) {
        caps_.EXT_disjoint_timer_query =
            load_proc(procs.glGenQueriesEXT, "glGenQueriesEXT") &&
            load_proc(procs.glDeleteQueriesEXT, "glDeleteQueriesEXT") &&
            load_proc(procs.glQueryCounterEXT, "glQueryCounterEXT") &&
            load_proc(procs.glGetQueryObjectivEXT, "glGetQueryObjectivEXT") &&
            load_proc(procs.glGetQueryObjectui64vEXT, "glGetQueryObjectui64vEXT") &&
            (load_proc(procs.glGetInteger64vEXT, "glGetInteger64vEXT") ||
             load_proc(procs.glGetInteger64vEXT, "glGetInteger64v"));
    }
    if (!caps_.OES_egl_image) {
        alog(Log::Error, "GL_OES_EGL_image unsupported");
        return false;
    }
    if (caps_.KHR_debug) {
        glEnable(GL_DEBUG_OUTPUT_KHR);
        glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS_KHR);
        procs.glDebugMessageCallbackKHR(gl_log, nullptr);
        procs.glDebugMessageControlKHR(GL_DONT_CARE, GL_DEBUG_TYPE_POP_GROUP_KHR, GL_DONT_CARE, 0, nullptr, GL_FALSE);
        procs.glDebugMessageControlKHR(GL_DONT_CARE, GL_DEBUG_TYPE_PUSH_GROUP_KHR, GL_DONT_CARE, 0, nullptr, GL_FALSE);
    }

    shaders_ = std::make_unique<ShaderLibrary>(caps_.OES_egl_image_external_essl3);
    if (!shaders_->load()) {
        alog(Log::Error, "The renderer's shaders don't compile");
        return false;
    }

    shm_formats(caps_, &shm_formats_);

    int fd = wlr_renderer_get_drm_fd(&hook_.base);
    uint64_t cap = 0;
    if (fd >= 0 && drmGetCap(fd, DRM_CAP_SYNCOBJ_TIMELINE, &cap) == 0)
        hook_.base.features.timeline = egl_->has_fences() && cap != 0;
    return true;
}

Renderer::~Renderer() {
    egl_->make_current();
    while (!textures.empty())
        texture_destroy(*textures.begin());
    // Imported framebuffers die with their buffers; drop what's left.
    while (!framebuffers.empty()) {
        Framebuffer* fb = *framebuffers.begin();
        if (fb->buffer) {
            delete fb;
        } else {
            framebuffers.erase(fb);  // a Target's: its owner frees it
        }
    }
    shaders_.reset();
    if (caps_.KHR_debug) {
        glDisable(GL_DEBUG_OUTPUT_KHR);
        procs.glDebugMessageCallbackKHR(nullptr, nullptr);
    }
    egl_->unset_current();
    wlr_drm_format_set_finish(&shm_formats_);
    if (drm_fd_ >= 0)
        close(drm_fd_);
}

bool Renderer::check_reset() {
    if (!procs.glGetGraphicsResetStatusKHR)
        return false;
    const GLenum status = procs.glGetGraphicsResetStatusKHR();
    if (status == GL_NO_ERROR)
        return false;
    alog(Log::Error, "GPU reset (%s)", reset_status(status));
    wl_signal_emit_mutable(&hook_.base.events.lost, nullptr);
    return true;
}

Framebuffer* Renderer::framebuffer_for(wlr_buffer* buffer) {
    if (wlr_addon* a = wlr_addon_find(&buffer->addons, this, &kFramebufferAddon))
        return wl_container_of(a, static_cast<Framebuffer*>(nullptr), addon);
    wlr_dmabuf_attributes dmabuf{};
    if (!wlr_buffer_get_dmabuf(buffer, &dmabuf))
        return nullptr;
    auto* fb = new Framebuffer();
    fb->renderer = this;
    fb->buffer = buffer;
    fb->width = buffer->width;
    fb->height = buffer->height;
    fb->image = egl_->import_dmabuf(dmabuf, &fb->external_only);
    if (fb->image == EGL_NO_IMAGE_KHR) {
        fb->buffer = nullptr;  // not added yet
        framebuffers.insert(fb);
        delete fb;
        return nullptr;
    }
    wlr_addon_init(&fb->addon, &buffer->addons, this, &kFramebufferAddon);
    framebuffers.insert(fb);
    return fb;
}

wlr_texture* Renderer::texture_from_buffer(wlr_buffer* buffer) {
    if (!egl_->make_current())
        return nullptr;
    void* data;
    uint32_t format;
    size_t stride;
    wlr_dmabuf_attributes dmabuf{};
    if (wlr_buffer_get_dmabuf(buffer, &dmabuf)) {
        Framebuffer* fb = framebuffer_for(buffer);
        if (!fb || !procs.glEGLImageTargetTexture2DOES)
            return nullptr;
        auto* t = new Texture();
        wlr_texture_init(&t->base, &hook_.base, &kTextureImpl, dmabuf.width, dmabuf.height);
        t->renderer = this;
        t->target = fb->external_only ? GL_TEXTURE_EXTERNAL_OES : GL_TEXTURE_2D;
        t->buffer = fb;
        t->has_alpha = drm_format_has_alpha(dmabuf.format);
        // External images show changes by themselves; others are rebound.
        const bool fresh = !fb->tex;
        if (fresh)
            glGenTextures(1, &fb->tex);
        if (fresh || !fb->external_only) {
            glBindTexture(t->target, fb->tex);
            glTexParameteri(t->target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(t->target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            procs.glEGLImageTargetTexture2DOES(t->target, fb->image);
            glBindTexture(t->target, 0);
        }
        t->tex = fb->tex;
        wlr_buffer_lock(buffer);
        textures.insert(t);
        return &t->base;
    }
    if (!wlr_buffer_begin_data_ptr_access(buffer, WLR_BUFFER_DATA_PTR_ACCESS_READ, &data, &format, &stride))
        return nullptr;
    const PixelFormat* f = format_from_drm(format);
    if (!f || !format_supported(caps_, *f) || stride % f->bytes_per_pixel ||
        stride < size_t(buffer->width) * f->bytes_per_pixel) {
        wlr_buffer_end_data_ptr_access(buffer);
        alog(Log::Error, "Can't upload shm format 0x%08x", format);
        return nullptr;
    }
    auto* t = new Texture();
    wlr_texture_init(&t->base, &hook_.base, &kTextureImpl, buffer->width, buffer->height);
    t->renderer = this;
    t->target = GL_TEXTURE_2D;
    t->has_alpha = f->has_alpha;
    t->drm_format = f->drm;
    glGenTextures(1, &t->tex);
    glBindTexture(GL_TEXTURE_2D, t->tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, GLint(stride / f->bytes_per_pixel));
    glTexImage2D(GL_TEXTURE_2D, 0, f->gl_internal ? f->gl_internal : f->gl_format, buffer->width, buffer->height,
                 0, f->gl_format, f->gl_type, data);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
    wlr_buffer_end_data_ptr_access(buffer);
    textures.insert(t);
    return &t->base;
}

} // namespace atrium::render
