#pragma once
// One frame drawn into one framebuffer. Coordinates are the target's pixels
// (y down); boxes are floats so scaled scene nodes land between pixels.
// Clip regions are whole pixels. Ported from scenefx's fx_pass.c (MIT).

#include "render/renderer.hpp"
#include "render/shaders.hpp"

namespace atrium::render {

struct FBox {
    double x = 0, y = 0, width = 0, height = 0;
    bool empty() const { return width <= 0 || height <= 0; }
    static FBox of(const wlr_box& b) { return {double(b.x), double(b.y), double(b.width), double(b.height)}; }
};

// Corner radii in pixels, clockwise from the top left.
struct Corners {
    float tl = 0, tr = 0, br = 0, bl = 0;
    bool empty() const { return tl <= 0 && tr <= 0 && br <= 0 && bl <= 0; }
    static Corners all(float r) { return {r, r, r, r}; }
};

// An area left out of what is drawn: a rounded box, for a window's shadow
// and outline not to show through its own translucent content.
struct CutOut {
    wlr_box area{};
    Corners corners;
    bool valid() const { return area.width > 0 && area.height > 0; }
};

struct TextureDraw {
    TexRef tex;
    wlr_fbox src{};  // texture pixels; empty: all of it
    FBox dst;
    wl_output_transform transform = WL_OUTPUT_TRANSFORM_NORMAL;
    const pixman_region32_t* clip = nullptr;  // null: dst
    float alpha = 1;
    wlr_scale_filter_mode filter = WLR_SCALE_FILTER_BILINEAR;
    bool blend = true;

    // Content that isn't plain SDR: its transfer function, gamut and how
    // much to scale its light so its reference white is SDR white.
    wlr_color_transfer_function transfer = wlr_color_transfer_function(0);
    const wlr_color_primaries* primaries = nullptr;
    float luminance = 1;

    wlr_drm_syncobj_timeline* wait_timeline = nullptr;
    uint64_t wait_point = 0;

    // Rounded corners over `round_box` (empty: dst).
    Corners corners;
    FBox round_box;
    CutOut cut;
    bool discard_transparent = false;
};

struct RectDraw {
    FBox box;
    float color[4] = {0, 0, 0, 1};  // premultiplied
    const pixman_region32_t* clip = nullptr;
    Corners corners;
    CutOut cut;
    bool blend = true;
};

struct ShadowDraw {
    FBox box;
    float color[4] = {0, 0, 0, 0.5f};
    float sigma = 0;
    float radius = 0;
    const pixman_region32_t* clip = nullptr;
    CutOut cut;
};

// The whole scene's blur settings.
struct BlurParams {
    int passes = 3;
    float radius = 5;
    float noise = 0.02f;
    float brightness = 0.9f;
    float contrast = 0.9f;
    float saturation = 1.1f;

    bool enabled() const { return passes > 0 && radius > 0; }
    bool has_effects() const { return noise > 0 || brightness != 1 || contrast != 1 || saturation != 1; }
    // How far the blur reaches, in pixels.
    int reach() const;
    // Weaker by `strength` (0-1): fewer passes, a smaller radius.
    BlurParams scaled(float strength) const;
};

// Liquid Glass's material (see glass.frag).
struct GlassMaterial {
    float tint[4] = {0, 0, 0, 0};
    float adapt = 0;
    float saturation = 1;
    float highlight = 0;
    float light_dir[2] = {0.7071f, 0.7071f};
    float shadow = 0;
    bool operator==(const GlassMaterial&) const = default;
};

// One glass shape, in target pixels: its box, corner radius, opacity and
// where it shows (clip width < 0: all of it).
struct GlassShapeDraw {
    float x, y, width, height, radius, opacity;
    float clip_x, clip_y, clip_width, clip_height;
};

struct BlurDraw {
    FBox box;            // what the blur covers
    const pixman_region32_t* clip = nullptr;
    Corners corners;
    CutOut cut;
    float alpha = 1;
    float strength = 1;
    // Use the cached blur of what's under the windows (BlurCache nodes)
    // rather than blurring what's drawn so far.
    bool use_cache = false;
    // A mask: only where it has alpha is blurred (a panel's buffer).
    const TexRef* mask = nullptr;
    wlr_fbox mask_src{};
    FBox mask_box;
    wl_output_transform mask_transform = WL_OUTPUT_TRANSFORM_NORMAL;

    // Liquid Glass when refraction > 0.
    float refraction = 0, thickness = 0;
    GlassMaterial glass;
    const GlassShapeDraw* shapes = nullptr;
    int shape_count = 0;
};

// Per-output offscreen targets for effects, kept between frames.
struct EffectBuffers {
    Target effects, effects_swapped;  // blur ping-pong
    Target cache_blurred;             // the cached blur of what's under windows
    Target cache_plain;               // ...and the same, unblurred
    Target saved;                     // pixels round the blur, put back over its artifacts
    Target blend;                     // the half-float frame (HDR)
    Target field, field_swapped;      // Liquid Glass's shape field
};

class RenderPass {
public:
    wlr_render_pass* wlr() { return &hook_.base; }
    static RenderPass* from(wlr_render_pass* p);

    int width() const { return width_; }
    int height() const { return height_; }

    // The per-output buffers effects need (PassOptions::effects); without
    // them blur is skipped.
    EffectBuffers* effects() const { return fx_; }
    Framebuffer* target() const { return fb_; }
    const BlurParams* blur_params = nullptr;

    void add_texture(const TextureDraw& d);
    void add_rect(const RectDraw& d);
    void add_shadow(const ShadowDraw& d);
    void add_blur(const BlurDraw& d);
    // Re-blurs what's drawn so far over `box` into the blur cache.
    bool render_blur_cache(const FBox& box);
    // Copies `region` of `src` over `dst` (pixels, same size).
    void copy(const pixman_region32_t* region, Framebuffer* dst, Framebuffer* src);

    // The user's screen shader over `region`, before submit.
    void apply_screen_shader(const pixman_region32_t* region);

    // Ends the frame: the colour pass, the timer, the fence. Frees this.
    bool submit();

private:
    RenderPass(Renderer& r, Framebuffer* fb, const PassOptions& o);
    ~RenderPass();
    friend class Renderer;

    Framebuffer* blur_into(const BlurParams& params, Framebuffer* source, const pixman_region32_t* region);
    void render_glass(Framebuffer* blurred, const BlurDraw& d);
    Framebuffer* glass_field(const BlurDraw& d, const TexRef* mask, const float mask_norm[4], float sigma);
    void output_pass();
    // Draws `box` (its unit square through the program's proj) where it
    // meets `clip`.
    void draw(Program& p, const FBox& box, const pixman_region32_t* clip);
    void set_proj(Program& p, const FBox& box);
    // What GL samples to read a framebuffer.
    TexRef sampler(Framebuffer* fb);
    void bind(Framebuffer* fb);

    struct Hook {
        wlr_render_pass base;
        RenderPass* self;
    };
    Hook hook_{};
    Renderer& r_;
    Framebuffer* fb_;         // what's drawn into now
    Framebuffer* output_fb_;  // the output's buffer when drawing into the blend buffer
    EffectBuffers* fx_ = nullptr;
    int width_, height_;
    float proj_[9];
    RenderTimer* timer_;
    wlr_drm_syncobj_timeline* signal_timeline_ = nullptr;
    uint64_t signal_point_ = 0;
    OutputColor color_;
    bool two_pass_ = false;
    wlr_buffer* locked_ = nullptr;
    Target own_blend_;  // the blend buffer when there are no EffectBuffers
};

} // namespace atrium::render
