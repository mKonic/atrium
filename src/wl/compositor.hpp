#pragma once
#include "wl/buffer.hpp"
#include "wl/region.hpp"
#include "wl/signal.hpp"

#include <deque>
#include <memory>
#include <vector>

namespace atrium::wl {

class Output;
class Subsurface;
class Surface;

// What a surface is for: an xdg toplevel, a subsurface, a cursor, a layer
// surface. A surface takes one kind of role for life; the object that gives
// it (an xdg_surface, a wl_subsurface) can go and come back as the same kind.
class Role {
public:
    virtual ~Role() = default;
    // The kind, compared by pointer: a static string per kind.
    virtual const char* name() const = 0;
    // The client committed: false (with a protocol error posted) rejects it.
    virtual bool precommit(Surface&) { return true; }
    // A state was applied to the surface.
    virtual void commit(Surface&) {}
};

// One wl_surface state, pending or applied (wl_surface's double buffering).
struct SurfaceState {
    enum Bit : uint32_t {
        Buffer = 1 << 0,
        Damage = 1 << 1,
        Opaque = 1 << 2,
        Input = 1 << 3,
        Transform = 1 << 4,
        Scale = 1 << 5,
        Offset = 1 << 6,
        Frame = 1 << 7,
        Viewport = 1 << 8,
        Subsurfaces = 1 << 9,
        XdgGeometry = 1 << 10,
        XdgAck = 1 << 11,
        XdgSizeLimits = 1 << 12,
        Layer = 1 << 13,
    };
    uint32_t committed = 0;

    BufferRef buffer;  // empty: no content (attached null, or never)
    int32_t dx = 0, dy = 0;  // how far the content moves with this buffer
    Region surface_damage, buffer_damage;
    Region opaque;
    Region input = Region::infinite();
    int32_t transform = WL_OUTPUT_TRANSFORM_NORMAL;
    int32_t scale = 1;
    std::vector<Weak<WlCallback>> frames;

    // wp_viewporter: the crop, in buffer coordinates after scale and
    // transform, and the size to show it at.
    struct Viewport {
        bool has_source = false, has_destination = false;
        double sx = 0, sy = 0, sw = 0, sh = 0;
        int dw = 0, dh = 0;
    } viewport;

    // xdg_surface / xdg_toplevel state, double-buffered with the surface's.
    struct {
        int x = 0, y = 0, width = 0, height = 0;  // window geometry; 0 wide: unset
    } xdg_geometry;
    uint32_t xdg_configure_serial = 0;  // the configure acked before this commit
    int min_width = 0, min_height = 0, max_width = 0, max_height = 0;

    // zwlr_layer_surface_v1 state, double-buffered the same way.
    struct LayerState {
        uint32_t anchor = 0;  // edge bits: 1 top, 2 bottom, 4 left, 8 right
        int32_t exclusive_zone = 0;
        uint32_t exclusive_edge = 0;
        int32_t margin_top = 0, margin_right = 0, margin_bottom = 0, margin_left = 0;
        uint32_t keyboard_interactive = 0;  // 0 none, 1 exclusive, 2 on demand
        uint32_t desired_width = 0, desired_height = 0;
        uint32_t layer = 0;  // background, bottom, top, overlay
        uint32_t configure_serial = 0;
        uint32_t actual_width = 0, actual_height = 0;  // what the acked configure said
        bool operator==(const LayerState&) const = default;
    } layer;

    // A child in stacking order, where it sits relative to its parent;
    // `sub` null is the parent surface itself.
    struct Placement {
        Subsurface* sub;
        int x, y;
    };
    std::vector<Placement> subsurfaces{{nullptr, 0, 0}};

    // Worked out when the state is applied.
    int width = 0, height = 0;  // surface coordinates
    int buffer_width = 0, buffer_height = 0;

    // Why an applied-but-queued state waits (Hyprland's lock reasons): its
    // parent's commit (a synchronized subsurface), a fence, a fifo barrier.
    enum Lock : uint32_t { LockSync = 1, LockFence = 2, LockFifo = 4, LockTimer = 8 };
    uint32_t locks = 0;

    // Folds a later state into this one, as if both were committed at once.
    void merge(SurfaceState&& later);
};

// The wlr_buffer a surface shows, carrying its texture: the renderer draws
// the texture; an shm buffer goes back to its client as soon as it has been
// copied in. Scene nodes take this as their buffer.
struct SurfaceBuffer {
    wlr_buffer base;
    wlr_texture* texture;
    BufferRef source;  // held while the texture reads from it (dmabuf)
    // Locks by the surface's own scene nodes, which want the next frame
    // anyway: while only they hold it, a commit updates the texture in
    // place. Anyone else's lock (a frozen copy) makes a new one.
    size_t ignore_locks = 0;

    static SurfaceBuffer* from(wlr_buffer* buffer);
};

class Compositor;

class Surface : public WlSurface {
public:
    Surface(wl_client* client, uint32_t version, uint32_t id, Compositor& compositor);
    ~Surface() override;

    static Surface* from(wl_resource* resource);

    const SurfaceState& current() const { return current_; }
    const SurfaceState& pending() const { return pending_; }
    // What the scene draws: null while there is no content.
    wlr_buffer* buffer() const { return shown_ ? &shown_->base : nullptr; }
    wlr_texture* texture() const { return shown_ ? shown_->texture : nullptr; }
    // The part of the buffer that changed with the last applied state.
    const Region& buffer_damage() const { return current_.buffer_damage; }
    // The part of the buffer shown (the viewport's crop), in buffer pixels.
    wlr_fbox source_box() const;
    bool accepts_input(double sx, double sy) const;

    // The role; see Role. set_role fails (posting `error` on `on`) when the
    // surface has another kind of role, or a live one of this kind.
    bool set_role(Role* role, Resource* on, uint32_t error);
    void clear_role(Role* role);
    Role* role() const { return role_; }
    const char* role_name() const { return role_name_; }

    // Mapped is the role's call (an xdg toplevel with a buffer, a subsurface
    // whose parent is mapped); these tell everyone.
    void map();
    void unmap();
    bool mapped() const { return mapped_; }

    // Subsurfaces, in stacking order (the current state's), with where they sit.
    const std::vector<SurfaceState::Placement>& children() const { return current_.subsurfaces; }
    Subsurface* subsurface() const { return subsurface_; }
    // The top of the subsurface tree this surface is in.
    Surface* root();

    // Frame callbacks: the surface was shown at `ms` (CLOCK_MONOTONIC).
    void send_frame_done(uint32_t ms);
    bool wants_frame() const { return !current_.frames.empty(); }

    void enter(Output& output);
    void leave(Output& output);
    const std::vector<Output*>& outputs() const { return outputs_; }
    void set_preferred_scale(int32_t scale);
    void set_preferred_transform(uint32_t transform);

    // Everything below the protocol's reach (extensions' pending state:
    // viewporter, fifo, syncobj) goes through here.
    SurfaceState& pending_state() { return pending_; }
    // Holds or releases a queued state (fences, fifo barriers).
    SurfaceState* newest_queued() { return queue_.empty() ? nullptr : queue_.back().get(); }
    void unlock(SurfaceState* state, uint32_t lock);

    struct {
        Signal<> precommit;  // pending state about to be committed
        Signal<> commit;  // a state was applied
        Signal<> map, unmap;
        Signal<Subsurface*> new_subsurface;
        Signal<> destroy;
    } events;

private:
    friend class Subsurface;
    void commit();
    void process_queue();
    void apply(SurfaceState& state);
    void update_texture(bool buffer_changed);
    bool synchronized() const;
    void release_children();

    Compositor& compositor_;
    SurfaceState pending_, current_;
    std::deque<std::unique_ptr<SurfaceState>> queue_;
    SurfaceBuffer* shown_ = nullptr;
    Role* role_ = nullptr;
    const char* role_name_ = nullptr;
    Subsurface* subsurface_ = nullptr;  // set when the role is a subsurface
    bool mapped_ = false;
    std::vector<Output*> outputs_;
    int32_t preferred_scale_ = 0;
    uint32_t preferred_transform_ = UINT32_MAX;
};

class Subsurface : public WlSubsurface, public Role {
public:
    static constexpr const char* kRole = "wl_subsurface";

    Subsurface(wl_client* client, uint32_t version, uint32_t id, Surface* surface, Surface* parent);
    ~Subsurface() override;

    const char* name() const override { return kRole; }
    void commit(Surface& surface) override;

    Surface* surface() const { return surface_; }
    Surface* parent() const { return parent_; }
    bool sync() const { return sync_; }
    // Where it sits in its parent, as applied.
    int x() const;
    int y() const;

private:
    friend class Surface;
    void place(Surface* sibling, bool above);
    void detach_from_parent();
    void update_mapped();

    Surface* surface_;
    Surface* parent_;
    bool sync_ = true;
    Signal<>::Connection parent_map_, parent_unmap_, parent_destroy_, surface_destroy_;
};

// wl_region: a set of rectangles a client builds to hand to a surface.
class RegionResource : public WlRegion {
public:
    RegionResource(wl_client* client, uint32_t version, uint32_t id);
    Region region;
};

// wl_compositor and wl_subcompositor.
class Compositor {
public:
    // `renderer` makes the textures (null in tests: surfaces keep state only).
    Compositor(wl_display* display, wlr_renderer* renderer);
    ~Compositor();
    Compositor(const Compositor&) = delete;
    Compositor& operator=(const Compositor&) = delete;

    wlr_renderer* renderer() const { return renderer_; }

    Signal<Surface*> new_surface;

private:
    wlr_renderer* renderer_;
    std::unique_ptr<Global> compositor_global_, subcompositor_global_;
    std::vector<Weak<WlCompositor>> compositors_;
    std::vector<Weak<WlSubcompositor>> subcompositors_;
};

} // namespace atrium::wl
