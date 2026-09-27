#pragma once
// atrium's scene graph: a tree of nodes (trees, rectangles, buffers,
// shadows, blur) with per-node damage and visibility, drawn onto outputs by
// atrium's renderer. Trees can scale and fade what's in them.
//
// The damage, visibility and output logic is ported from wlroots' wlr_scene
// (MIT) by way of scenefx (MIT), which added the effect nodes; the node
// tree follows KWin's item tree and Hyprland's render pass in spirit.

#include "listener.hpp"
#include "render/pass.hpp"
#include "wlr.hpp"

#include <cstdint>
#include <functional>

namespace atrium::scene {

class Blur;
class Buffer;
class Scene;
class SceneOutput;
class SurfaceNode;
class Tree;
struct OutputAddonAccess;

enum class Type { Tree, Rect, Buffer, Shadow, BlurCache, Blur };

// Corner radii in logical pixels, clockwise from the top left.
struct Radii {
    int tl = 0, tr = 0, br = 0, bl = 0;
    static Radii all(int r) { return {r, r, r, r}; }
    static Radii top(int r) { return {r, r, 0, 0}; }
    static Radii bottom(int r) { return {0, 0, r, r}; }
    bool empty() const { return tl <= 0 && tr <= 0 && br <= 0 && bl <= 0; }
    bool operator==(const Radii&) const = default;
};

// A rounded area (node-local) left out of what a node draws.
struct CutOut {
    wlr_box area{};
    Radii corners;
    bool empty() const { return area.width <= 0 || area.height <= 0; }
    bool operator==(const CutOut& o) const {
        return corners == o.corners && area.x == o.area.x && area.y == o.area.y && area.width == o.area.width &&
               area.height == o.area.height;
    }
};

// Where a node lands in the layout, through its trees' scale and opacity.
struct Placement {
    double x = 0, y = 0;
    double scale = 1;
    float opacity = 1;
    bool enabled = true;
};

class Node {
public:
    Node(const Node&) = delete;
    Node& operator=(const Node&) = delete;

    const Type type;
    Tree* parent;
    wl_list link;  // parent->children
    bool enabled = true;
    int x = 0, y = 0;
    void* data = nullptr;
    // Where it shows, in layout coordinates.
    pixman_region32_t visible;
    struct {
        wl_signal destroy;
    } events;
    wlr_addon_set addons;

    // Destroys this node and all under it.
    void destroy();
    void set_enabled(bool enabled);
    void set_position(int x, int y);
    void place_above(Node* sibling);
    void place_below(Node* sibling);
    void raise_to_top();
    void lower_to_bottom();
    void reparent(Tree* parent);
    // Layout coordinates (rounded), and whether it and every ancestor are
    // enabled.
    bool coords(int* lx, int* ly) const;
    Placement placement() const;
    Scene* root() const;

    // Every enabled buffer node under this one, with its layout position.
    void for_each_buffer(const std::function<void(Buffer*, int lx, int ly)>& fn);
    // The topmost node taking input at a layout point, and the point in its
    // own coordinates.
    Node* at(double lx, double ly, double* nx = nullptr, double* ny = nullptr);

    // Its own size (0 for trees), unscaled.
    void size(int* w, int* h) const;

protected:
    Node(Type type, Tree* parent);
    virtual ~Node();
    // Recomputes visibility where the node was and is, and damages it.
    void update(pixman_region32_t* damage = nullptr);

    friend class Scene;
    friend class SceneOutput;
    friend struct SceneImpl;
};

class Tree : public Node {
public:
    static Tree* create(Tree* parent);

    wl_list children;  // Node::link, bottom to top

    // What's in the tree drawn scaled about its origin (1: as is), and
    // faded by `opacity`.
    float scale() const { return scale_; }
    float opacity() const { return opacity_; }
    void set_scale(float scale);
    void set_opacity(float opacity);

protected:
    explicit Tree(Tree* parent, Type type = Type::Tree);
    ~Tree() override;

private:
    float scale_ = 1;
    float opacity_ = 1;
    friend class Node;
};

class Rect : public Node {
public:
    static Rect* create(Tree* parent, int width, int height, const float color[4]);
    void set_size(int width, int height);
    void set_color(const float color[4]);
    void set_corner_radius(int r) { set_corner_radii(Radii::all(r)); }
    void set_corner_radii(Radii r);
    void set_cut_out(const CutOut& cut);

    int width, height;
    float color[4];  // premultiplied
    Radii corners;
    bool accepts_input = true;
    CutOut cut;

private:
    Rect(Tree* parent, int width, int height, const float color[4]);
};

class Shadow : public Node {
public:
    static Shadow* create(Tree* parent, int width, int height, int corner_radius, float blur_sigma,
                          const float color[4]);
    void set_size(int width, int height);
    void set_corner_radius(int r);
    void set_blur_sigma(float sigma);
    void set_color(const float color[4]);
    void set_cut_out(const CutOut& cut);

    int width, height;
    int corner_radius;
    float blur_sigma;
    float color[4];
    CutOut cut;

private:
    Shadow(Tree* parent, int width, int height, int corner_radius, float sigma, const float color[4]);
};

// Liquid Glass's material, and its exact shapes (atrium-glass-v1).
using GlassMaterial = render::GlassMaterial;
struct GlassShape {
    float x, y, width, height, radius, opacity;
    float clip_x, clip_y, clip_width, clip_height;
    bool operator==(const GlassShape&) const = default;
};
constexpr int kMaxGlassShapes = 16;

class Blur : public Node {
public:
    static Blur* create(Tree* parent, int width, int height);
    void set_size(int width, int height);
    void set_corner_radius(int r) { set_corner_radii(Radii::all(r)); }
    void set_corner_radii(Radii r);
    // Blur the cached blur of what's under all windows (BlurCache), not
    // everything drawn below.
    void set_use_cache(bool use_cache);
    // Only where this buffer has pixels is blurred (a panel's surface).
    void set_mask(Buffer* mask);
    Buffer* mask() const { return mask_; }
    void set_alpha(float alpha);
    void set_strength(float strength);
    void set_refraction(float refraction, float thickness);
    void set_glass(const GlassMaterial& glass);
    void set_glass_shapes(const GlassShape* shapes, int count);
    void set_cut_out(const CutOut& cut);

    int width, height;
    Radii corners;
    CutOut cut;
    float strength = 1;
    float alpha = 1;
    float refraction = 0, thickness = 0;
    GlassMaterial glass;
    GlassShape shapes[kMaxGlassShapes]{};
    int shape_count = 0;
    bool use_cache = false;

private:
    Blur(Tree* parent, int width, int height);
    ~Blur() override;
    Buffer* mask_ = nullptr;
    friend class Buffer;
};

// The blur of what's under the windows (wallpaper and background layers),
// kept between frames and made again only when marked dirty: Blur nodes
// with use_cache read it instead of blurring every frame.
class BlurCache : public Node {
public:
    static BlurCache* create(Tree* parent, int width, int height);
    void set_size(int width, int height);
    void mark_dirty();

    int width, height;
    bool dirty = false;

private:
    BlurCache(Tree* parent, int width, int height);
};

struct OutputsUpdateEvent {
    SceneOutput** active;
    size_t size;
};

struct OutputSampleEvent {
    SceneOutput* output;
    bool direct_scanout;
    wlr_drm_syncobj_timeline* release_timeline;
    uint64_t release_point;
};

struct FrameDoneEvent {
    SceneOutput* output;
    timespec when;
};

struct BufferOptions {
    const pixman_region32_t* damage = nullptr;  // buffer-local; null: all of it
    wlr_drm_syncobj_timeline* wait_timeline = nullptr;
    uint64_t wait_point = 0;
};

class Buffer : public Node {
public:
    static Buffer* create(Tree* parent, wlr_buffer* buffer);

    void set_buffer(wlr_buffer* buffer, const BufferOptions& options = BufferOptions());
    void set_opaque_region(const pixman_region32_t* region);
    void set_source_box(const wlr_fbox* box);
    void set_dest_size(int width, int height);
    void set_transform(wl_output_transform transform);
    void set_opacity(float opacity);
    void set_filter_mode(wlr_scale_filter_mode mode);
    void set_transfer_function(wlr_color_transfer_function tf);
    void set_primaries(wlr_color_named_primaries primaries);
    void set_color_encoding(wlr_color_encoding encoding);
    void set_color_range(wlr_color_range range);
    void set_corner_radius(int r) { set_corner_radii(Radii::all(r)); }
    void set_corner_radii(Radii r);
    void send_frame_done(FrameDoneEvent* event);

    // The surface this shows, when it's a surface's node.
    SurfaceNode* surface() const { return surface_; }

    struct {
        wl_signal outputs_update;  // OutputsUpdateEvent
        wl_signal output_enter;    // SceneOutput
        wl_signal output_leave;    // SceneOutput
        wl_signal output_sample;   // OutputSampleEvent
        wl_signal frame_done;      // FrameDoneEvent
    } events;

    // Whether a point (node-local, adjusted in place) takes input.
    bool (*point_accepts_input)(Buffer* buffer, double* sx, double* sy) = nullptr;

    wlr_buffer* buffer = nullptr;
    SceneOutput* primary_output = nullptr;
    float opacity = 1;
    wlr_scale_filter_mode filter_mode = WLR_SCALE_FILTER_BILINEAR;
    wlr_fbox src_box{};
    int dst_width = 0, dst_height = 0;
    wl_output_transform transform = WL_OUTPUT_TRANSFORM_NORMAL;
    pixman_region32_t opaque_region;
    wlr_color_transfer_function transfer_function = wlr_color_transfer_function(0);
    wlr_color_named_primaries primaries = wlr_color_named_primaries(0);
    wlr_color_encoding color_encoding = WLR_COLOR_ENCODING_NONE;
    wlr_color_range color_range = WLR_COLOR_RANGE_NONE;
    Radii corners;

private:
    Buffer(Tree* parent, wlr_buffer* buffer);
    ~Buffer() override;
    void take_buffer(wlr_buffer* buffer);
    void set_texture(wlr_texture* texture);
    wlr_texture* texture(wlr_renderer* renderer);
    bool is_black_opaque() const;

    uint64_t active_outputs_ = 0;
    wlr_texture* texture_ = nullptr;
    wlr_linux_dmabuf_feedback_v1_init_options prev_feedback_{};
    bool own_buffer_ = false;
    int buffer_width_ = 0, buffer_height_ = 0;
    bool buffer_is_opaque_ = false;
    wlr_drm_syncobj_timeline* wait_timeline_ = nullptr;
    uint64_t wait_point_ = 0;
    Listener<> buffer_release_;
    Listener<> renderer_destroy_;
    bool single_pixel_ = false;
    uint32_t single_pixel_color_[4]{};
    Blur* mask_of_ = nullptr;
    SurfaceNode* surface_ = nullptr;

    friend class Node;
    friend class Blur;
    friend class SceneOutput;
    friend class SurfaceNode;
    friend struct SceneImpl;
};

class Scene : public Tree {
public:
    static Scene* create();

    // The blur Blur and BlurCache nodes use.
    void set_blur(const render::BlurParams& params);
    const render::BlurParams& blur() const { return blur_; }

    void set_linux_dmabuf_v1(wlr_linux_dmabuf_v1* dmabuf);
    void set_gamma_control_manager_v1(wlr_gamma_control_manager_v1* gamma);
    void set_color_manager_v1(wlr_color_manager_v1* manager);

    SceneOutput* output_for(wlr_output* output);

    wl_list outputs;  // SceneOutput::link
    wlr_linux_dmabuf_v1* linux_dmabuf_v1 = nullptr;
    wlr_gamma_control_manager_v1* gamma_control_manager_v1 = nullptr;
    wlr_color_manager_v1* color_manager_v1 = nullptr;

    bool restack_xwayland_surfaces = true;
    bool direct_scanout = true;
    bool calculate_visibility = true;
    enum class DebugDamage { None, Rerender, Highlight } debug_damage = DebugDamage::None;

private:
    Scene();
    ~Scene() override;
    render::BlurParams blur_;
    // A BlurCache made again since visibility was last computed: what it
    // forced visible below it can be culled again.
    bool blur_cache_rendered_ = false;
    Listener<> dmabuf_destroy_, gamma_destroy_, color_destroy_;
    Listener<wlr_gamma_control_manager_v1_set_gamma_event> gamma_set_;

    friend class Node;
    friend class SceneOutput;
    friend struct SceneImpl;
};

// Time spent making a frame: CPU before rendering, then the GPU's.
struct Timer {
    int64_t pre_render_duration = 0;
    wlr_render_timer* render_timer = nullptr;
    int64_t duration_ns();
    void finish();
};

class SceneOutput {
public:
    static SceneOutput* create(Scene* scene, wlr_output* output);
    void destroy();

    void set_position(int lx, int ly);
    // HDR: SDR content's white in nits (0: the default reference white).
    void set_sdr_white_nits(float nits);
    // HDR: the gamut SDR content is spread over (null: sRGB).
    void set_sdr_primaries(const wlr_color_primaries* primaries);
    // A white point tint in linear light (night light); 1, 1, 1 for none.
    void set_tint(float r, float g, float b);

    struct StateOptions {
        Timer* timer = nullptr;
        wlr_swapchain* swapchain = nullptr;
    };
    bool needs_frame() const;
    bool commit(const StateOptions* options = nullptr);
    bool build_state(wlr_output_state* state, const StateOptions* options = nullptr);
    void send_frame_done(const timespec* now);
    void for_each_buffer(const std::function<void(Buffer*, int lx, int ly)>& fn);

    wlr_output* output;
    wl_list link;  // Scene::outputs
    Scene* scene;
    wlr_damage_ring damage_ring;
    int x = 0, y = 0;
    uint8_t index = 0;
    struct {
        wl_signal destroy;
    } events;
    // Damage not yet committed to the output.
    pixman_region32_t pending_commit_damage;

private:
    SceneOutput(Scene* scene, wlr_output* output);
    ~SceneOutput();
    void damage(const pixman_region32_t* damage);
    void damage_whole();
    void update_geometry(bool force);
    void attempt_gamma(wlr_output_state* state);
    render::OutputColor output_color(const wlr_output_image_description* desc) const;

    wlr_addon addon_{};
    render::EffectBuffers fx_;
    uint8_t dmabuf_feedback_debounce_ = 0;
    bool prev_scanout_ = false;
    bool gamma_lut_changed_ = false;
    wlr_gamma_control_v1* gamma_lut_ = nullptr;
    wlr_color_transform* gamma_lut_transform_ = nullptr;
    float sdr_white_nits_ = 0;
    float tint_[3] = {1, 1, 1};
    wlr_color_primaries sdr_primaries_{};
    bool sdr_primaries_set_ = false;
    bool color_changed_ = false;
    wlr_drm_syncobj_timeline* in_timeline_ = nullptr;
    uint64_t in_point_ = 0;
    wlr_drm_syncobj_timeline* out_timeline_ = nullptr;
    uint64_t out_point_ = 0;
    struct Highlight {
        pixman_region32_t region;
        timespec when;
    };
    std::vector<Highlight*> highlights_;
    Listener<wlr_output_event_commit> commit_;
    Listener<wlr_output_event_damage> damage_;
    Listener<> needs_frame_;
    Listener<> renderer_destroy_;

    friend class Node;
    friend class Buffer;
    friend class Scene;
    friend struct SceneImpl;
    friend struct OutputAddonAccess;
};

// ---- surfaces (ported from wlroots' types/scene helpers) ------------------

// A surface's buffer node, kept in step with the surface's commits.
class SurfaceNode {
public:
    static SurfaceNode* create(Tree* parent, wlr_surface* surface);
    void send_frame_done(const timespec* when);
    void set_clip(const wlr_box* clip);

    Buffer* buffer;
    wlr_surface* surface;
    wlr_box clip{};

private:
    SurfaceNode(Buffer* buffer, wlr_surface* surface);
    ~SurfaceNode();
    void reconfigure();
    Listener<OutputsUpdateEvent> outputs_update_;
    Listener<OutputSampleEvent> output_sample_;
    Listener<FrameDoneEvent> frame_done_;
    Listener<> surface_destroy_;
    Listener<> surface_commit_;
    friend class Buffer;
};

// A surface and its sub-surfaces, each a tree of its own.
Tree* subsurface_tree_create(Tree* parent, wlr_surface* surface);
// Clips a subsurface tree (found under `node`) to a box, surface-local.
void subsurface_tree_set_clip(Node* node, const wlr_box* clip);
// An xdg surface: its subsurface tree, offset by its window geometry, and
// placed at a popup's position.
Tree* xdg_surface_create(Tree* parent, wlr_xdg_surface* xdg_surface);
Tree* drag_icon_create(Tree* parent, wlr_drag_icon* icon);

struct LayerSurfaceNode {
    Tree* tree;
    wlr_layer_surface_v1* layer_surface;
};
LayerSurfaceNode* layer_surface_v1_create(Tree* parent, wlr_layer_surface_v1* layer_surface);
// Places it by its anchors and margins within `full_area` (or `usable_area`
// unless it asks for all of it), and takes its exclusive zone out of
// `usable_area`.
void layer_surface_v1_configure(LayerSurfaceNode* node, const wlr_box* full_area, wlr_box* usable_area);

// A capture source (ext-image-capture) of a subtree, for per-window
// screen sharing.
wlr_ext_image_capture_source_v1* capture_source_create(Node* node, wl_event_loop* loop, wlr_allocator* allocator,
                                                       wlr_renderer* renderer);

} // namespace atrium::scene
