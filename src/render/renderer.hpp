#pragma once
// atrium's renderer: GLES 3 on its own EGL context, drawing the scene into
// the output's swapchain buffers. It is a wlr_renderer too, so wlroots'
// pieces that still render (the compositor's shm uploads, output cursors,
// screencopy) go through it. The GLES 2 renderer of wlroots (MIT) and
// scenefx's (MIT) were the reference for the parts wlroots needs.

#include "render/egl.hpp"
#include "render/formats.hpp"

#include <memory>
#include <unordered_set>
#include <vector>

namespace atrium::backend {
class Backend;
}

namespace atrium::render {

class Renderer;
class RenderPass;
class ShaderLibrary;
struct Framebuffer;

// Something GL can sample: a wlr_texture of ours, or an offscreen target.
struct TexRef {
    GLenum target = GL_TEXTURE_2D;
    GLuint tex = 0;
    int width = 0, height = 0;
    bool has_alpha = true;
    // Set when these pixels are an HDR screen's signal (see Framebuffer).
    const Framebuffer* encoded = nullptr;
};

// Where GL draws: a wlr_buffer imported through its dmabuf, or a GL-only
// texture (offscreen targets: blur, the HDR blend buffer).
struct Framebuffer {
    Renderer* renderer = nullptr;
    wlr_buffer* buffer = nullptr;  // null: GL-only
    wlr_addon addon{};
    int width = 0, height = 0;
    bool external_only = false;
    EGLImageKHR image = EGL_NO_IMAGE_KHR;
    GLuint rbo = 0, fbo = 0, tex = 0, stencil = 0;
    GLenum internal_format = 0;  // GL-only: GL_RGBA8, GL_RGB10_A2, GL_RGBA16F

    // Written by the HDR output pass: the pixels are the screen's signal
    // (`encoded_tf` as output.frag's out_tf, 0 for plain SDR), and
    // `encoded_matrix` takes their linear light back to SDR's (1.0 = SDR
    // white, sRGB primaries). Copies (screenshots) are converted back.
    int encoded_tf = 0;
    float encoded_matrix[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};

    // The framebuffer object, made on first use (0 if it can't be).
    GLuint get_fbo();
    void bind();
    TexRef texture() const;

    ~Framebuffer();
};

// A GL-only render target, (re)allocated to a size: `ensure` keeps it when
// the size and format match.
class Target {
public:
    Target() = default;
    ~Target();
    Target(const Target&) = delete;
    Target& operator=(const Target&) = delete;
    // False if GL can't make it.
    bool ensure(Renderer& r, int width, int height, GLenum internal_format);
    void release();
    Framebuffer* get() { return fb_.get(); }
    Framebuffer* operator->() { return fb_.get(); }
    explicit operator bool() const { return bool(fb_); }

private:
    std::unique_ptr<Framebuffer> fb_;
};

// A wlr_texture of ours.
struct Texture {
    wlr_texture base;  // first: a wlr_texture* is a Texture*
    Renderer* renderer = nullptr;
    GLenum target = GL_TEXTURE_2D;
    // Imported textures borrow `buffer`'s texture; uploaded ones own theirs.
    GLuint tex = 0;
    GLuint fbo = 0;  // made for reading pixels back
    bool has_alpha = true;
    uint32_t drm_format = 0;         // uploads: how `update` reads data
    Framebuffer* buffer = nullptr;  // dmabuf imports

    TexRef ref() const;
};

// Colour work at the end of a frame: `matrix` over linear light, then the
// transfer function (output.frag's out_tf: 0 gamma 2.2, 1 PQ, 2 linear,
// 3 sRGB). Anything but plain gamma 2.2/sRGB draws through a half-float
// blend buffer and converts at submit.
// A display's colour profile as a 3D table (icc::Lut), looked up after the
// SDR encode: `size`³ RGB, red fastest. Uploaded on first use.
struct ColorLut {
    int size = 0;
    std::vector<float> rgb;
    GLuint tex = 0;
};

struct OutputColor {
    float matrix[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    int tf = 0;
    ColorLut* lut = nullptr;  // SDR only
    bool plain() const;
};

struct EffectBuffers;

struct PassOptions {
    // Offscreen buffers for blur and the colour pass, kept per output.
    // Without them there is no blur, and a colour pass uses a buffer of
    // its own.
    EffectBuffers* effects = nullptr;
    wlr_render_timer* timer = nullptr;
    wlr_drm_syncobj_timeline* signal_timeline = nullptr;
    uint64_t signal_point = 0;
    OutputColor color;
};

struct RenderTimer {
    wlr_render_timer base;  // first
    Renderer* renderer = nullptr;
    timespec cpu_start{}, cpu_end{};
    GLuint query = 0;
    GLint64 gl_cpu_end = 0;
};

class Renderer {
public:
    // A renderer on the backend's GPU, or null.
    static Renderer* create(const backend::Backend& backend);
    // A renderer on exactly this GPU (`drm_fd` stays the caller's); with
    // `software_ok`, Mesa's software rasterizer will do (a display-only GPU).
    static Renderer* create_on(int drm_fd, bool software_ok = false);
    // Ours, or null if `r` is some other wlr_renderer.
    static Renderer* from(wlr_renderer* r);
    static Texture* texture(wlr_texture* t);

    wlr_renderer* wlr() { return &hook_.base; }
    Egl& egl() { return *egl_; }
    ShaderLibrary& shaders() { return *shaders_; }
    const GlCaps& caps() const { return caps_; }

    Framebuffer* framebuffer_for(wlr_buffer* buffer);
    wlr_texture* texture_from_buffer(wlr_buffer* buffer);
    // A pass drawing into `fb` (locking its buffer); submit() or
    // wlr_render_pass_submit() ends and frees it.
    RenderPass* begin(Framebuffer* fb, const PassOptions& options);

    // Whether the GPU was reset under us (then `events.lost` fired).
    bool check_reset();

    struct {
        PFNGLEGLIMAGETARGETTEXTURE2DOESPROC glEGLImageTargetTexture2DOES = nullptr;
        PFNGLEGLIMAGETARGETRENDERBUFFERSTORAGEOESPROC glEGLImageTargetRenderbufferStorageOES = nullptr;
        PFNGLDEBUGMESSAGECALLBACKKHRPROC glDebugMessageCallbackKHR = nullptr;
        PFNGLDEBUGMESSAGECONTROLKHRPROC glDebugMessageControlKHR = nullptr;
        PFNGLGETGRAPHICSRESETSTATUSKHRPROC glGetGraphicsResetStatusKHR = nullptr;
        PFNGLGENQUERIESEXTPROC glGenQueriesEXT = nullptr;
        PFNGLDELETEQUERIESEXTPROC glDeleteQueriesEXT = nullptr;
        PFNGLQUERYCOUNTEREXTPROC glQueryCounterEXT = nullptr;
        PFNGLGETQUERYOBJECTIVEXTPROC glGetQueryObjectivEXT = nullptr;
        PFNGLGETQUERYOBJECTUI64VEXTPROC glGetQueryObjectui64vEXT = nullptr;
        PFNGLGETINTEGER64VEXTPROC glGetInteger64vEXT = nullptr;
    } procs;

    // Bookkeeping for Framebuffer/Texture lifetimes.
    std::unordered_set<Framebuffer*> framebuffers;
    std::unordered_set<Texture*> textures;

private:
    Renderer() = default;
    ~Renderer();
    bool init();

    struct Hook {
        wlr_renderer base;
        Renderer* self;
    };
    Hook hook_{};
    std::unique_ptr<Egl> egl_;
    std::unique_ptr<ShaderLibrary> shaders_;
    GlCaps caps_;
    wlr_drm_format_set shm_formats_{};
    int drm_fd_ = -1;

    friend struct RendererImpl;
};

} // namespace atrium::render
