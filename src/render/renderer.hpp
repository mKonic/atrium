#pragma once
// atrium's renderer: GLES 3 on its own EGL context, drawing the scene into
// the output's swapchain buffers, and uploading or importing clients'
// buffers as textures. The GLES 2 renderer of wlroots (MIT) and scenefx's
// (MIT) were the reference; its entry points keep wlroots' shapes.

#include "render/egl.hpp"
#include "render/formats.hpp"
#include "util/box.hpp"
#include "util/buffer.hpp"

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

// Something GL can sample: a Texture, or an offscreen target.
struct TexRef {
    GLenum target = GL_TEXTURE_2D;
    GLuint tex = 0;
    int width = 0, height = 0;
    bool has_alpha = true;
    // Set when these pixels are an HDR screen's signal (see Framebuffer).
    const Framebuffer* encoded = nullptr;
};

// Where GL draws: a Buffer imported through its dmabuf, or a GL-only
// texture (offscreen targets: blur, the HDR blend buffer).
struct Framebuffer {
    Renderer* renderer = nullptr;
    Buffer* buffer = nullptr;  // null: GL-only
    Addon addon{};
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

enum ScaleFilter { SCALE_FILTER_BILINEAR, SCALE_FILTER_NEAREST };
enum BlendMode { BLEND_MODE_PREMULTIPLIED, BLEND_MODE_NONE };

// Reading a texture's pixels into memory (screenshots, a CPU copy).
struct ReadPixelsOptions {
    void* data = nullptr;
    uint32_t format = 0;  // DRM_FORMAT_*
    uint32_t stride = 0;
    uint32_t dst_x = 0, dst_y = 0;  // where in `data`
    Box src_box;                    // empty: all of it
};

// Pixels GL samples: an uploaded shm buffer or an imported dmabuf.
struct Texture {
    int width = 0, height = 0;
    Renderer* renderer = nullptr;
    GLenum target = GL_TEXTURE_2D;
    // Imported textures borrow `buffer`'s texture; uploaded ones own theirs.
    GLuint tex = 0;
    GLuint fbo = 0;  // made for reading pixels back
    bool has_alpha = true;
    uint32_t drm_format = 0;         // uploads: how `update` reads data
    Framebuffer* buffer = nullptr;  // dmabuf imports

    TexRef ref() const;
    void destroy();
    bool read_pixels(const ReadPixelsOptions* options);
    // The format reading is cheapest in (DRM_FORMAT_INVALID: none).
    uint32_t preferred_read_format();
    // Its pixels again from `buffer` (an upload of the same format) where
    // `damage` says; false if it can't be.
    bool update_from_buffer(Buffer* buffer, const pixman_region32_t* damage);
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
// A screen's transfer function as OutputColor::tf.
int output_tf(wlr_color_transfer_function tf);

struct EffectBuffers;

struct RenderTimer;

struct PassOptions {
    // Offscreen buffers for blur and the colour pass, kept per output.
    // Without them there is no blur, and a colour pass uses a buffer of
    // its own.
    EffectBuffers* effects = nullptr;
    RenderTimer* timer = nullptr;
    wlr_drm_syncobj_timeline* signal_timeline = nullptr;
    uint64_t signal_point = 0;
    OutputColor color;
};

// How long the GPU took for a pass (GL_EXT_disjoint_timer_query).
struct RenderTimer {
    Renderer* renderer = nullptr;
    timespec cpu_start{}, cpu_end{};
    GLuint query = 0;
    GLint64 gl_cpu_end = 0;
    // -1 while unknown.
    int duration_ns();
    void destroy();
};

// A pass into a buffer (begin_buffer_pass), wlroots' options.
struct BufferPassOptions {
    OutputColor color;  // the colour work at the end (an HDR screen's signal)
    RenderTimer* timer = nullptr;
    wlr_drm_syncobj_timeline* signal_timeline = nullptr;  // signalled when the GPU is done
    uint64_t signal_point = 0;
};

struct Color {
    float r = 0, g = 0, b = 0, a = 1;
};

// A texture drawn by a pass (RenderPass::add_texture), wlroots' options.
struct TextureOptions {
    Texture* texture = nullptr;
    FBox src_box;  // texture pixels; empty: all of it
    Box dst_box;   // empty: the texture's size
    const float* alpha = nullptr;
    const pixman_region32_t* clip = nullptr;
    wl_output_transform transform = WL_OUTPUT_TRANSFORM_NORMAL;
    ScaleFilter filter_mode = SCALE_FILTER_BILINEAR;
    BlendMode blend_mode = BLEND_MODE_PREMULTIPLIED;
    wlr_drm_syncobj_timeline* wait_timeline = nullptr;
    uint64_t wait_point = 0;
    wlr_color_transfer_function transfer_function = wlr_color_transfer_function(0);
    const wlr_color_primaries* primaries = nullptr;
    const float* luminance_multiplier = nullptr;
};

// A filled rectangle (RenderPass::add_rect); colour premultiplied.
struct RectOptions {
    Box box;
    Color color;
    const pixman_region32_t* clip = nullptr;
    BlendMode blend_mode = BLEND_MODE_PREMULTIPLIED;
};

class Renderer {
public:
    // A renderer on the backend's GPU, or null.
    static Renderer* create(const backend::Backend& backend);
    // A renderer on exactly this GPU (`drm_fd` stays the caller's); with
    // `software_ok`, Mesa's software rasterizer will do (a display-only GPU).
    static Renderer* create_on(int drm_fd, bool software_ok = false);
    // Frees it (its textures and imported framebuffers with it).
    void destroy();

    Egl& egl() { return *egl_; }
    ShaderLibrary& shaders() { return *shaders_; }
    const GlCaps& caps() const { return caps_; }

    Framebuffer* framebuffer_for(Buffer* buffer);
    Texture* texture_from_buffer(Buffer* buffer);
    Texture* texture_from_pixels(uint32_t drm_format, uint32_t stride, uint32_t width, uint32_t height,
                                 const void* data);
    Texture* texture_from_dmabuf(const DmabufAttributes* attribs);
    // A pass drawing into `fb` (locking its buffer); submit() ends and frees it.
    RenderPass* begin(Framebuffer* fb, const PassOptions& options);
    // The same into a buffer, with wlroots' options.
    RenderPass* begin_buffer_pass(Buffer* buffer, const BufferPassOptions* options);
    // Null without timer queries.
    RenderTimer* timer_create();

    // The device it renders on (its render node), owned by it.
    int drm_fd();
    // What it can sample from buffers with `caps` (BUFFER_CAP_DMABUF: dmabufs;
    // BUFFER_CAP_DATA_PTR: uploads), and draw into.
    const FormatSet* texture_formats(uint32_t caps);
    const FormatSet* render_formats();

    struct {
        bool timeline = false;  // signal and wait timelines work (explicit sync)
    } features;
    struct {
        wl_signal lost;     // the GPU was reset: everything must be made again
        wl_signal destroy;  // it is going
    } events;

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

    std::unique_ptr<Egl> egl_;
    std::unique_ptr<ShaderLibrary> shaders_;
    GlCaps caps_;
    FormatSet shm_formats_{};
    int drm_fd_ = -1;
};

} // namespace atrium::render
