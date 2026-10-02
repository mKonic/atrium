#pragma once
// atrium's scene graph: a tree of nodes (trees, rectangles, buffers,
// shadows, blur) with per-node damage and visibility, drawn onto outputs by
// atrium's renderer. Trees can scale and fade what's in them.
//
// The damage, visibility and output logic is ported from wlroots' wlr_scene
// (MIT) by way of scenefx (MIT), which added the effect nodes; the node
// tree follows KWin's item tree and Hyprland's render pass in spirit.

#include "backend/output.hpp"
#include "listener.hpp"
#include "render/pass.hpp"
#include "wl/dmabuf.hpp"
#include "wl/signal.hpp"
#include "util/damage_ring.hpp"
#include "wlr.hpp"

#include <optional>
#include <vector>

#include <cstdint>
#include <functional>
#include <unordered_set>
#include <unordered_map>
#include <memory>

namespace atrium::wl {
class ColorManagement;
class FractionalScales;
class GammaControls;
class LayerSurface;
class LinuxDmabuf;
class Output;
class ShellSurface;
class Surface;
class Syncobj;
} // namespace atrium::wl

namespace atrium::scene {

class Blur;
class Buffer;
class Scene;
class SceneOutput;
class SurfaceNode;
class Tree;

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
    Box area{};
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
    AddonSet addons;

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
    // Drawn warped: `fn` takes a point of `frame` (a layout box; u, v in
    // 0..1) to where it lands. Its buffers are drawn as meshes; the rest of
    // it (shadows, outlines, blur) sits the warp out. Clear with an empty fn.
    void set_warp(std::function<std::pair<double, double>(double, double)> fn, FBox frame);
    const std::function<std::pair<double, double>(double, double)>& warp() const { return warp_; }
    const FBox& warp_frame() const { return warp_frame_; }

protected:
    explicit Tree(Tree* parent, Type type = Type::Tree);
    ~Tree() override;

private:
    float scale_ = 1;
    float opacity_ = 1;
    std::function<std::pair<double, double>(double, double)> warp_;
    FBox warp_frame_{};
    friend class Node;
};

// A tree's children, bottom to top or top to bottom; the current one may
// be removed. Not wl_list_for_each: at the end it makes a Node* of the list
// head and reads `link` through it, which UBSan's vptr check (Node is
// polymorphic) dereferences.
Node* node_of_link(wl_list* link);
class ChildRange {
public:
    ChildRange(const Tree* tree, bool top_down) : head_(const_cast<wl_list*>(&tree->children)), top_down_(top_down) {}
    class iterator {
    public:
        iterator(wl_list* cur, bool top_down) : cur_(cur), next_(top_down ? cur->prev : cur->next), top_down_(top_down) {}
        Node* operator*() const { return node_of_link(cur_); }
        iterator& operator++() {
            cur_ = next_;
            next_ = top_down_ ? cur_->prev : cur_->next;
            return *this;
        }
        bool operator!=(const iterator& o) const { return cur_ != o.cur_; }

    private:
        wl_list* cur_;
        wl_list* next_;
        bool top_down_;
    };
    iterator begin() const { return {top_down_ ? head_->prev : head_->next, top_down_}; }
    iterator end() const { return {head_, top_down_}; }

private:
    wl_list* head_;
    bool top_down_;
};
inline ChildRange each_child(const Tree* tree) { return {tree, false}; }
inline ChildRange each_child_top_down(const Tree* tree) { return {tree, true}; }

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
    Timeline* release_timeline;
    uint64_t release_point;
};

struct FrameDoneEvent {
    SceneOutput* output;
    timespec when;
};

struct BufferOptions {
    const pixman_region32_t* damage = nullptr;  // buffer-local; null: all of it
    Timeline* wait_timeline = nullptr;
    uint64_t wait_point = 0;
};

class Buffer : public Node {
public:
    static Buffer* create(Tree* parent, atrium::Buffer* buffer);

    void set_buffer(atrium::Buffer* buffer, const BufferOptions& options = BufferOptions());
    void set_opaque_region(const pixman_region32_t* region);
    void set_source_box(const FBox* box);
    void set_dest_size(int width, int height);
    void set_transform(wl_output_transform transform);
    void set_opacity(float opacity);
    void set_filter_mode(render::ScaleFilter mode);
    void set_transfer_function(TransferFunction tf);
    void set_primaries(NamedPrimaries primaries);
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

    atrium::Buffer* buffer = nullptr;
    SceneOutput* primary_output = nullptr;
    float opacity = 1;
    render::ScaleFilter filter_mode = render::SCALE_FILTER_BILINEAR;
    FBox src_box{};
    int dst_width = 0, dst_height = 0;
    wl_output_transform transform = WL_OUTPUT_TRANSFORM_NORMAL;
    pixman_region32_t opaque_region;
    TransferFunction transfer_function = TransferFunction(0);
    NamedPrimaries primaries = NamedPrimaries(0);
    Radii corners;
    // The dmabuf feedback last sent its surface: for scan-out on that
    // output, or (null) for rendering.
    std::optional<std::pair<backend::Output*, bool>> feedback_sent;

private:
    Buffer(Tree* parent, atrium::Buffer* buffer);
    ~Buffer() override;
    void take_buffer(atrium::Buffer* buffer);
    void set_texture(render::Texture* texture);
    render::Texture* texture(render::Renderer* renderer);
    bool is_black_opaque() const;
    void note_single_pixel(atrium::Buffer* b);

    uint64_t active_outputs_ = 0;
    render::Texture* texture_ = nullptr;
    bool own_buffer_ = false;
    int buffer_width_ = 0, buffer_height_ = 0;
    bool buffer_is_opaque_ = false;
    Timeline* wait_timeline_ = nullptr;
    uint64_t wait_point_ = 0;
    Listener<> buffer_release_;
    Listener<> renderer_destroy_;
    bool single_pixel_ = false;
    float single_pixel_color_[4]{};
    Blur* mask_of_ = nullptr;
    SurfaceNode* surface_ = nullptr;

    friend class Node;
    friend class Blur;
    friend class SceneOutput;
    friend class SurfaceNode;
    friend struct SceneImpl;
};

struct Protocols {
    wl::LinuxDmabuf* dmabuf = nullptr;
    // The feedback for a surface on `scanout` (null: rendered only).
    std::function<wl::DmabufFeedback(backend::Output* scanout)> dmabuf_feedback;
    wl::FractionalScales* fractional_scales = nullptr;
    wl::ColorManagement* color = nullptr;
    wl::Syncobj* syncobj = nullptr;
};

class Scene : public Tree {
public:
    static Scene* create();

    // The blur Blur and BlurCache nodes use.
    void set_blur(const render::BlurParams& params);
    const render::BlurParams& blur() const { return blur_; }

    // The protocols surfaces are told things through (null: not there).
    // The compositor sets them, and clears them before they go.
    Protocols protocols;
    void set_gamma_controls(wl::GammaControls* gamma);

    SceneOutput* output_for(const backend::Output* output);
    // Draws the pointer into a frame where the screen has no cursor plane.
    std::function<void(const backend::Output*, render::RenderPass*, const pixman_region32_t*)> draw_cursor;
    SceneOutput* output_for(const wl::Output* output);

    wl_list outputs;  // SceneOutput::link

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
    wl::GammaControls* gamma_ = nullptr;
    wl::Connection gamma_set_;

    friend class Node;
    friend class SceneOutput;
    friend struct SceneImpl;
};

// Time spent making a frame: CPU before rendering, then the GPU's.
struct Timer {
    int64_t pre_render_duration = 0;
    render::RenderTimer* render_timer = nullptr;
    int64_t duration_ns();
    void finish();
};

class SceneOutput {
public:
    static SceneOutput* create(Scene* scene, backend::Output* output);
    void destroy();

    void set_position(int lx, int ly);
    // HDR: SDR content's white in nits (0: the default reference white).
    void set_sdr_white_nits(float nits);
    // HDR: the gamut SDR content is spread over (null: sRGB).
    void set_sdr_primaries(const ColorPrimaries* primaries);
    // A white point tint in linear light (night light); 1, 1, 1 for none.
    void set_tint(float r, float g, float b);
    // The display's colour profile as a 3D table (SDR), or none.
    void set_color_lut(std::unique_ptr<render::ColorLut> lut);

    struct StateOptions {
        Timer* timer = nullptr;
        backend::Swapchain* swapchain = nullptr;
    };
    // Everything drawn again next frame.
    void damage_whole();
    bool needs_frame() const;
    bool commit(const StateOptions* options = nullptr);
    bool build_state(backend::OutputState* state, const StateOptions* options = nullptr);
    void send_frame_done(const timespec* now);
    void for_each_buffer(const std::function<void(Buffer*, int lx, int ly)>& fn);

    // Surfaces' content shown this frame: their presentation feedbacks,
    // sent when the output says it was presented.
    void presentation_pending(std::vector<std::shared_ptr<void>> feedbacks, bool zero_copy);

    backend::Output* output;
    wl::Output* global = nullptr;  // its wl_output (the compositor sets it)
    wl_list link;  // Scene::outputs
    Scene* scene;
    DamageRing damage_ring;
    int x = 0, y = 0;
    uint8_t index = 0;
    struct {
        wl_signal destroy;
    } events;
    // Damage not yet committed to the output.
    pixman_region32_t pending_commit_damage;

private:
    SceneOutput(Scene* scene, backend::Output* output);
    ~SceneOutput();
    void damage(const pixman_region32_t* damage);
    void update_geometry(bool force);
    void attempt_gamma(backend::OutputState* state);
    render::OutputColor output_color(const backend::ImageDescription* desc) const;

    render::EffectBuffers fx_;
    std::unique_ptr<render::ColorLut> lut_;
    // Offscreen layers of warped trees, kept while they stay warped.
    std::unordered_map<const Tree*, std::unique_ptr<render::Target>> warp_layers_;
    std::unordered_set<const Tree*> warp_layers_used_;
    void drop_lut_texture();
    uint8_t dmabuf_feedback_debounce_ = 0;
    bool prev_scanout_ = false;
    bool gamma_lut_changed_ = false;
    ColorTransform* gamma_lut_transform_ = nullptr;
    struct Feedbacks {
        std::vector<std::shared_ptr<void>> list;
        bool zero_copy;
    };
    std::vector<Feedbacks> sampled_;
    struct Committed {
        uint32_t seq;
        Feedbacks feedbacks;
    };
    std::vector<Committed> committed_;
    float sdr_white_nits_ = 0;
    float tint_[3] = {1, 1, 1};
    ColorPrimaries sdr_primaries_{};
    bool sdr_primaries_set_ = false;
    bool color_changed_ = false;
    Timeline* in_timeline_ = nullptr;
    uint64_t in_point_ = 0;
    Timeline* out_timeline_ = nullptr;
    uint64_t out_point_ = 0;
    struct Highlight {
        pixman_region32_t region;
        timespec when;
    };
    std::vector<Highlight*> highlights_;
    wl::Connection commit_, present_, damage_, needs_frame_, output_destroy_;
    Listener<> renderer_destroy_;

    friend class Node;
    friend class Buffer;
    friend class Scene;
    friend struct SceneImpl;
};

// ---- surfaces (ported from wlroots' types/scene helpers) ------------------

// A surface's buffer node, kept in step with the surface's commits.
class SurfaceNode {
public:
    static SurfaceNode* create(Tree* parent, wl::Surface* surface);
    void send_frame_done(const timespec* when);
    void set_clip(const Box* clip);

    Buffer* buffer;
    wl::Surface* surface;
    Box clip{};

private:
    SurfaceNode(Buffer* buffer, wl::Surface* surface);
    ~SurfaceNode();
    void reconfigure();
    void outputs_changed(SceneOutput** active, size_t n);
    SceneOutput* pacing_output() const;

    std::vector<SceneOutput*> on_;  // the outputs it was last shown on
    bool suspended_ = false;        // shown on none just now
    Listener<OutputsUpdateEvent> outputs_update_;
    Listener<OutputSampleEvent> output_sample_;
    Listener<FrameDoneEvent> frame_done_;
    wl::Connection surface_destroy_, surface_commit_;
    friend class Buffer;
};

// A surface and its sub-surfaces, each a tree of its own.
Tree* subsurface_tree_create(Tree* parent, wl::Surface* surface);
// Clips a subsurface tree (found under `node`) to a box, surface-local.
void subsurface_tree_set_clip(Node* node, const Box* clip);
// An xdg surface: its subsurface tree, offset by its window geometry, and
// placed at a popup's position.
Tree* xdg_surface_create(Tree* parent, wl::ShellSurface* xdg_surface);
Tree* drag_icon_create(Tree* parent, wl::Surface* icon);

struct LayerSurfaceNode {
    Tree* tree;
    wl::LayerSurface* layer_surface;
};
LayerSurfaceNode* layer_surface_v1_create(Tree* parent, wl::LayerSurface* layer_surface);
// Places it by its anchors and margins within `full_area` (or `usable_area`
// unless it asks for all of it), and takes its exclusive zone out of
// `usable_area`.
void layer_surface_v1_configure(LayerSurfaceNode* node, const Box* full_area, Box* usable_area);

// A subtree drawn on a private output of its own, sized to what is in it:
// for capturing one window. Goes with the node.
class CaptureSource {
public:
    static CaptureSource* create(Node* node, wl_event_loop* loop, backend::Allocator* allocator, render::Renderer* renderer);
    void destroy();

    // Someone watches: frames are drawn (counted, as sessions come and go).
    void start();
    void stop();
    // A frame wanted: drawn now if `force`, else once something changes.
    void request_frame(bool force);
    // The size frames come in (0 before the first).
    int width() const;
    int height() const;

    std::function<void(atrium::Buffer* buffer, const pixman_region32_t* damage, const timespec& when)> on_frame;
};

} // namespace atrium::scene
