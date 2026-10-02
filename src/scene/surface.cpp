// Surfaces in the scene: ported from wlroots' types/scene (surface.c,
// subsurface_tree.c, xdg_shell.c, layer_shell_v1.c, drag_icon.c; MIT), on
// atrium's own protocol layer.

#include "scene/internal.hpp"

#include "wl/color.hpp"
#include "wl/dmabuf.hpp"
#include "wl/layer_shell.hpp"
#include "wl/output.hpp"
#include "wl/surface_ext.hpp"
#include "wl/timing.hpp"
#include "wl/xdg_shell.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>

namespace atrium::scene {

namespace {

// wp_color_manager_v1's named values to the renderer's.
wlr_color_transfer_function tf_of(uint32_t wp) {
    switch (wp) {
    case 1: return WLR_COLOR_TRANSFER_FUNCTION_BT1886;
    case 5: return WLR_COLOR_TRANSFER_FUNCTION_EXT_LINEAR;
    case 9: return WLR_COLOR_TRANSFER_FUNCTION_SRGB;
    case 11: return WLR_COLOR_TRANSFER_FUNCTION_ST2084_PQ;
    default: return WLR_COLOR_TRANSFER_FUNCTION_GAMMA22;
    }
}

wlr_color_named_primaries primaries_of(uint32_t wp) {
    return wp == 6 ? WLR_COLOR_NAMED_PRIMARIES_BT2020 : WLR_COLOR_NAMED_PRIMARIES_SRGB;
}

uint32_t wp_tf(wlr_color_transfer_function tf) {
    switch (tf) {
    case WLR_COLOR_TRANSFER_FUNCTION_BT1886: return 1;
    case WLR_COLOR_TRANSFER_FUNCTION_EXT_LINEAR: return 5;
    case WLR_COLOR_TRANSFER_FUNCTION_SRGB: return 9;
    case WLR_COLOR_TRANSFER_FUNCTION_ST2084_PQ: return 11;
    default: return 2;
    }
}

int tf_preference(wlr_color_transfer_function tf) {
    switch (tf) {
    case WLR_COLOR_TRANSFER_FUNCTION_GAMMA22: return 0;
    case WLR_COLOR_TRANSFER_FUNCTION_ST2084_PQ: return 1;
    default: return -1;
    }
}

bool alive(Scene* scene, SceneOutput* o) {
    SceneOutput* s;
    wl_list_for_each(s, &scene->outputs, link)
        if (s == o)
            return true;
    return false;
}

void unmark_client_buffer(Buffer* b) {
    if (!b->buffer)
        return;
    if (wl::SurfaceBuffer* sb = wl::SurfaceBuffer::from(b->buffer); sb && sb->ignore_locks > 0)
        --sb->ignore_locks;
}

bool surface_accepts_input(Buffer* b, double* sx, double* sy) {
    SurfaceNode* s = b->surface();
    *sx += s->clip.x;
    *sy += s->clip.y;
    return s->surface->accepts_input(*sx, *sy);
}

uint32_t ms_of(const timespec& t) {
    return uint32_t(int64_t(t.tv_sec) * 1000 + t.tv_nsec / 1000000);
}

} // namespace

// ---- SurfaceNode ------------------------------------------------------------

SurfaceNode::SurfaceNode(Buffer* b, wl::Surface* s) : buffer(b), surface(s) {}

SurfaceNode::~SurfaceNode() { unmark_client_buffer(buffer); }

// The output that paces the surface's frames (callbacks, presentation): of
// those it is on, the fastest.
SceneOutput* SurfaceNode::pacing_output() const {
    if (suspended_)
        return nullptr;
    Scene* scene = buffer->root();
    SceneOutput* best = nullptr;
    for (SceneOutput* o : on_)
        if (alive(scene, o) && (!best || o->output->refresh > best->output->refresh))
            best = o;
    return best;
}

void SurfaceNode::outputs_changed(SceneOutput** active, size_t n) {
    Scene* scene = buffer->root();
    // Seen on no output: keep what was last sent, so a surface hidden and
    // shown again on the same output isn't told to leave and enter.
    if (n == 0) {
        suspended_ = true;
        return;
    }
    suspended_ = false;
    std::vector<SceneOutput*> next(active, active + n);
    for (SceneOutput* o : on_)
        if (alive(scene, o) && o->global && std::ranges::find(next, o) == next.end())
            surface->leave(*o->global);
    for (SceneOutput* o : next)
        if (o->global)
            surface->enter(*o->global);
    on_ = std::move(next);

    double scale = 1;
    wlr_color_transfer_function tf = WLR_COLOR_TRANSFER_FUNCTION_GAMMA22;
    bool wide = false;
    for (SceneOutput* o : on_) {
        scale = std::max(scale, double(o->output->scale));
        if (const auto& d = o->output->image_description) {
            if (tf_preference(tf) < tf_preference(d->transfer_function))
                tf = d->transfer_function;
            wide = wide || d->primaries == WLR_COLOR_NAMED_PRIMARIES_BT2020;
        }
    }
    const Protocols& p = scene->protocols;
    if (p.fractional_scales)
        p.fractional_scales->set_preferred_scale(surface, scale);
    surface->set_preferred_scale(int32_t(std::ceil(scale)));
    if (p.color) {
        wl::ImageDescription d;
        d.tf_named = wp_tf(tf);
        d.primaries_named = wide ? 6 : 1;
        p.color->set_preferred(surface, d);
    }
}

SurfaceNode* SurfaceNode::create(Tree* parent, wl::Surface* surface) {
    Buffer* b = Buffer::create(parent, nullptr);
    auto* sn = new SurfaceNode(b, surface);
    b->surface_ = sn;
    b->point_accepts_input = surface_accepts_input;

    sn->outputs_update_.connect(&b->events.outputs_update,
                                [sn](OutputsUpdateEvent* e) { sn->outputs_changed(e->active, e->size); });
    sn->output_sample_.connect(&b->events.output_sample, [sn](OutputSampleEvent* e) {
        if (sn->pacing_output() != e->output)
            return;
        // Its presentation feedbacks go out once the output says when.
        e->output->presentation_pending(sn->surface->take_feedbacks(), e->direct_scanout);
        if (e->release_timeline && sn->buffer->root()->protocols.syncobj)
            sn->buffer->root()->protocols.syncobj->add_release_point(sn->surface, e->release_timeline,
                                                                     e->release_point);
    });
    sn->frame_done_.connect(&b->events.frame_done, [sn](FrameDoneEvent* e) {
        if (sn->pacing_output() == e->output)
            sn->surface->send_frame_done(ms_of(e->when));
    });
    sn->surface_destroy_ = surface->events.destroy.connect([sn] { sn->buffer->destroy(); });
    sn->surface_commit_ = surface->events.commit.connect([sn] {
        sn->reconfigure();
        // A frame asked for: schedule one where it would be seen.
        int lx, ly;
        const bool on = sn->buffer->coords(&lx, &ly);
        SceneOutput* out = sn->pacing_output();
        if (sn->surface->wants_frame() && out && on)
            out->output->schedule_frame();
    });
    sn->reconfigure();
    return sn;
}

void SurfaceNode::send_frame_done(const timespec* when) {
    if (pixman_region32_not_empty(&buffer->visible))
        surface->send_frame_done(ms_of(*when));
}

void SurfaceNode::set_clip(const Box* c) {
    const Box next = c ? *c : Box{};
    if (box_equal(&next, &clip))
        return;
    clip = next;
    reconfigure();
}

void SurfaceNode::reconfigure() {
    Buffer* b = buffer;
    const wl::SurfaceState& state = surface->current();
    FBox src = surface->source_box();
    pixman_region32_t opaque;
    pixman_region32_init(&opaque);
    pixman_region32_copy(&opaque, state.opaque.get());
    int width = state.width, height = state.height;

    if (!box_empty(&clip)) {
        int bw = state.buffer_width, bh = state.buffer_height;
        const auto t = wl_output_transform(state.transform);
        width = std::min(clip.width, width - clip.x);
        height = std::min(clip.height, height - clip.y);
        fbox_transform(&src, &src, t, bw, bh);
        output_transform_coords(t, &bw, &bh);
        src.x += double(clip.x) * src.width / state.width;
        src.y += double(clip.y) * src.height / state.height;
        src.width *= double(width) / state.width;
        src.height *= double(height) / state.height;
        fbox_transform(&src, &src, output_transform_invert(t), bw, bh);
        pixman_region32_translate(&opaque, -clip.x, -clip.y);
        pixman_region32_intersect_rect(&opaque, &opaque, 0, 0, unsigned(std::max(0, width)),
                                       unsigned(std::max(0, height)));
    }
    if (width <= 0 || height <= 0) {
        b->set_buffer(nullptr);
        pixman_region32_fini(&opaque);
        return;
    }

    wlr_color_transfer_function tf = WLR_COLOR_TRANSFER_FUNCTION_GAMMA22;
    wlr_color_named_primaries primaries = WLR_COLOR_NAMED_PRIMARIES_SRGB;
    if (const auto& d = state.image_description) {
        tf = tf_of(d->tf_named);
        primaries = primaries_of(d->primaries_named);
    }

    b->set_opaque_region(&opaque);
    b->set_source_box(&src);
    b->set_dest_size(width, height);
    b->set_transform(wl_output_transform(state.transform));
    b->set_opacity(state.alpha);
    b->set_transfer_function(tf);
    b->set_primaries(primaries);
    unmark_client_buffer(b);

    if (atrium::Buffer* wb = surface->buffer()) {
        // In-place texture updates are fine unless the colour of a
        // single-pixel buffer was cached.
        wl::SurfaceBuffer* sb = wl::SurfaceBuffer::from(wb);
        float rgba[4];
        const bool single_pixel = sb && wl::SinglePixelBuffers::color_of(sb->source.get(), rgba);
        if (sb && !single_pixel)
            ++sb->ignore_locks;
        BufferOptions o;
        o.damage = surface->buffer_damage().get();
        if (state.sync.acquire) {
            o.wait_timeline = state.sync.acquire.get();
            o.wait_point = state.sync.acquire_point;
        }
        b->set_buffer(wb, o);
    } else {
        b->set_buffer(nullptr);
    }
    pixman_region32_fini(&opaque);
}

// ---- subsurface trees -------------------------------------------------------

namespace {

struct SubsurfaceTree;

// Each tree knows its node's tree; a surface knows its trees by parent.
std::unordered_map<const Node*, SubsurfaceTree*>& trees_by_node() {
    static std::unordered_map<const Node*, SubsurfaceTree*> m;
    return m;
}

struct SubsurfaceTree {
    Tree* tree = nullptr;
    wl::Surface* surface = nullptr;
    SurfaceNode* node = nullptr;
    SubsurfaceTree* parent = nullptr;  // null for the top surface
    Box clip{};
    std::unordered_map<wl::Subsurface*, SubsurfaceTree*> children;
    Listener<> tree_destroy;
    wl::Connection surface_destroy, surface_commit, surface_map, surface_unmap;
};

bool reconfigure_clip(SubsurfaceTree* t) {
    if (t->parent)
        t->clip = {t->parent->clip.x - t->tree->x, t->parent->clip.y - t->tree->y, t->parent->clip.width,
                   t->parent->clip.height};
    Buffer* b = t->node->buffer;
    if (box_empty(&t->clip)) {
        t->node->set_clip(nullptr);
        b->set_enabled(true);
        b->set_position(0, 0);
        return false;
    }
    Box clip = t->clip;
    const Box surface_box{0, 0, t->surface->current().width, t->surface->current().height};
    const bool meets = box_intersection(&clip, &clip, &surface_box);
    b->set_enabled(meets);
    if (meets) {
        b->set_position(clip.x, clip.y);
        t->node->set_clip(&clip);
    }
    return true;
}

SubsurfaceTree* surface_tree_create(Tree* parent, wl::Surface* surface);

SubsurfaceTree* create_child(SubsurfaceTree* parent, wl::Subsurface* sub) {
    SubsurfaceTree* c = surface_tree_create(parent->tree, sub->surface());
    c->parent = parent;
    parent->children[sub] = c;
    return c;
}

void reconfigure(SubsurfaceTree* t) {
    const bool clipped = reconfigure_clip(t);
    // Children in stacking order; `sub` null is the surface itself.
    Node* prev = nullptr;
    for (const wl::SurfaceState::Placement& p : t->surface->children()) {
        Node* n;
        if (!p.sub) {
            n = t->node->buffer;
        } else {
            auto it = t->children.find(p.sub);
            // A new subsurface at a gone one's address: not that one's tree.
            if (it != t->children.end() && it->second->surface != p.sub->surface()) {
                it->second->tree->destroy();
                it = t->children.end();
            }
            SubsurfaceTree* c = it != t->children.end() ? it->second : create_child(t, p.sub);
            c->tree->set_position(p.x, p.y);
            if (clipped)
                reconfigure_clip(c);
            n = c->tree;
        }
        if (prev)
            n->place_above(prev);
        prev = n;
    }
}

SubsurfaceTree* surface_tree_create(Tree* parent, wl::Surface* surface) {
    auto* t = new SubsurfaceTree();
    t->tree = Tree::create(parent);
    t->node = SurfaceNode::create(t->tree, surface);
    t->surface = surface;
    trees_by_node()[t->tree] = t;
    t->tree_destroy.connect(&t->tree->events.destroy, [t](void*) {
        // Its tree and node go with the scene node.
        trees_by_node().erase(t->tree);
        if (t->parent)
            std::erase_if(t->parent->children, [t](const auto& kv) { return kv.second == t; });
        for (auto& [sub, c] : t->children)
            c->parent = nullptr;
        delete t;
    });
    reconfigure(t);

    t->surface_destroy = surface->events.destroy.connect([t] { t->tree->destroy(); });
    // TODO(upstream too): only on a change of order or position.
    t->surface_commit = surface->events.commit.connect([t] {
        // A subsurface gone from the list: its tree goes.
        std::vector<SubsurfaceTree*> stale;
        for (auto& [sub, c] : t->children)
            if (std::ranges::none_of(t->surface->children(), [sub](const auto& p) { return p.sub == sub; }))
                stale.push_back(c);
        for (SubsurfaceTree* c : stale)
            c->tree->destroy();
        reconfigure(t);
    });
    t->surface_map = surface->events.map.connect([t] { t->tree->set_enabled(true); });
    t->surface_unmap = surface->events.unmap.connect([t] { t->tree->set_enabled(false); });
    t->tree->set_enabled(surface->mapped());
    return t;
}

bool set_clip(Node* node, const Box* clip) {
    if (node->type != Type::Tree)
        return false;
    bool found = false;
    if (auto it = trees_by_node().find(node); it != trees_by_node().end()) {
        SubsurfaceTree* t = it->second;
        if (!t->parent) {
            const Box next = clip ? *clip : Box{};
            if (box_equal(&t->clip, &next))
                return true;
            t->clip = next;
        }
        found = true;
        reconfigure_clip(t);
    }
    for (Node* child : each_child(static_cast<Tree*>(node)))
        found = set_clip(child, clip) || found;
    return found;
}

} // namespace

Tree* subsurface_tree_create(Tree* parent, wl::Surface* surface) {
    return surface_tree_create(parent, surface)->tree;
}

void subsurface_tree_set_clip(Node* node, const Box* clip) {
    [[maybe_unused]] const bool found = set_clip(node, clip);
    assert(found);
}

// ---- xdg surfaces -------------------------------------------------------------

namespace {

struct XdgSurfaceNode {
    Tree* tree;
    Tree* surface_tree;
    wl::ShellSurface* xdg;
    Listener<> tree_destroy;
    wl::Connection xdg_destroy, commit;

    void update_position() {
        const Box g = xdg->geometry();
        surface_tree->set_position(-g.x, -g.y);
        if (wl::Popup* p = xdg->popup())
            tree->set_position(p->geometry().x, p->geometry().y);
    }
};

} // namespace

Tree* xdg_surface_create(Tree* parent, wl::ShellSurface* xdg) {
    auto* x = new XdgSurfaceNode();
    x->xdg = xdg;
    x->tree = Tree::create(parent);
    x->surface_tree = subsurface_tree_create(x->tree, xdg->surface());
    x->tree_destroy.connect(&x->tree->events.destroy, [x](void*) { delete x; });
    x->xdg_destroy = xdg->events.destroy.connect([x] { x->tree->destroy(); });
    x->commit = xdg->surface()->events.commit.connect([x] { x->update_position(); });
    x->update_position();
    return x->tree;
}

// ---- layer surfaces -----------------------------------------------------------

namespace {

constexpr uint32_t kTop = 1, kBottom = 2, kLeft = 4, kRight = 8;

struct LayerNodeImpl {
    LayerSurfaceNode pub;
    Listener<> tree_destroy;
    wl::Connection layer_destroy;
};

// The edge its exclusive zone is taken from (wlroots'
// wlr_layer_surface_v1_get_exclusive_edge).
uint32_t exclusive_edge(const wl::LayerSurface::State& s) {
    if (s.exclusive_zone <= 0)
        return 0;
    if (s.exclusive_edge != 0 || s.anchor == (kTop | kBottom | kLeft | kRight))
        return s.exclusive_edge;
    switch (s.anchor) {
    case kTop:
    case kTop | kLeft | kRight: return kTop;
    case kBottom:
    case kBottom | kLeft | kRight: return kBottom;
    case kLeft:
    case kLeft | kTop | kBottom: return kLeft;
    case kRight:
    case kRight | kTop | kBottom: return kRight;
    default: return 0;
    }
}

void take_exclusive_zone(const wl::LayerSurface::State& s, uint32_t edge, Box* usable) {
    switch (edge) {
    case kTop:
        usable->y += s.exclusive_zone + s.margin_top;
        usable->height -= s.exclusive_zone + s.margin_top;
        break;
    case kBottom:
        usable->height -= s.exclusive_zone + s.margin_bottom;
        break;
    case kLeft:
        usable->x += s.exclusive_zone + s.margin_left;
        usable->width -= s.exclusive_zone + s.margin_left;
        break;
    case kRight:
        usable->width -= s.exclusive_zone + s.margin_right;
        break;
    default:
        return;
    }
    usable->width = std::max(usable->width, 0);
    usable->height = std::max(usable->height, 0);
}

} // namespace

LayerSurfaceNode* layer_surface_v1_create(Tree* parent, wl::LayerSurface* layer) {
    auto* l = new LayerNodeImpl();
    l->pub.layer_surface = layer;
    l->pub.tree = Tree::create(parent);
    subsurface_tree_create(l->pub.tree, layer->surface());
    l->tree_destroy.connect(&l->pub.tree->events.destroy, [l](void*) { delete l; });
    l->layer_destroy = layer->events.destroy.connect([l] { l->pub.tree->destroy(); });
    return &l->pub;
}

void layer_surface_v1_configure(LayerSurfaceNode* node, const Box* full_area, Box* usable_area) {
    wl::LayerSurface* layer = node->layer_surface;
    const wl::LayerSurface::State& s = layer->current();
    // Exclusive zone -1: the whole output, else what's left of it.
    const Box bounds = s.exclusive_zone == -1 ? *full_area : *usable_area;
    Box box{0, 0, int(s.desired_width), int(s.desired_height)};
    const uint32_t a = s.anchor;
    if (box.width == 0) {
        box.x = bounds.x + s.margin_left;
        box.width = bounds.width - (s.margin_left + s.margin_right);
    } else if ((a & kLeft) && (a & kRight)) {
        box.x = bounds.x + bounds.width / 2 - box.width / 2;
    } else if (a & kLeft) {
        box.x = bounds.x + s.margin_left;
    } else if (a & kRight) {
        box.x = bounds.x + bounds.width - box.width - s.margin_right;
    } else {
        box.x = bounds.x + bounds.width / 2 - box.width / 2;
    }
    if (box.height == 0) {
        box.y = bounds.y + s.margin_top;
        box.height = bounds.height - (s.margin_top + s.margin_bottom);
    } else if ((a & kTop) && (a & kBottom)) {
        box.y = bounds.y + bounds.height / 2 - box.height / 2;
    } else if (a & kTop) {
        box.y = bounds.y + s.margin_top;
    } else if (a & kBottom) {
        box.y = bounds.y + bounds.height - box.height - s.margin_bottom;
    } else {
        box.y = bounds.y + bounds.height / 2 - box.height / 2;
    }
    node->tree->set_position(box.x, box.y);
    layer->configure(uint32_t(std::max(0, box.width)), uint32_t(std::max(0, box.height)));
    if (layer->surface()->mapped() && s.exclusive_zone > 0)
        take_exclusive_zone(s, exclusive_edge(s), usable_area);
}

// ---- drag icons ---------------------------------------------------------------

namespace {

struct DragIconNode {
    Tree* tree;
    Tree* surface_tree;
    wl::Surface* icon;
    Listener<> tree_destroy;
    wl::Connection commit, icon_destroy;
};

} // namespace

Tree* drag_icon_create(Tree* parent, wl::Surface* icon) {
    auto* d = new DragIconNode();
    d->icon = icon;
    d->tree = Tree::create(parent);
    d->surface_tree = subsurface_tree_create(d->tree, icon);
    d->tree_destroy.connect(&d->tree->events.destroy, [d](void*) { delete d; });
    d->commit = icon->events.commit.connect([d] {
        const wl::SurfaceState& s = d->icon->current();
        d->surface_tree->set_position(d->surface_tree->x + s.dx, d->surface_tree->y + s.dy);
    });
    d->icon_destroy = icon->events.destroy.connect([d] { d->tree->destroy(); });
    return d->tree;
}

} // namespace atrium::scene
