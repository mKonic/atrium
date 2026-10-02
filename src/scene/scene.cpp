#include "scene/internal.hpp"

#include "wl/compositor.hpp"
#include "wl/desktop.hpp"
#include "wl/surface_ext.hpp"
#ifdef ATRIUM_XWAYLAND
#include "xwayland/xwm.hpp"
#endif

#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace atrium::scene {

namespace {

bool env_true(const char* name) {
    const char* v = std::getenv(name);
    return v && (std::strcmp(v, "1") == 0 || std::strcmp(v, "true") == 0);
}

uint32_t region_area(const pixman_region32_t* region) {
    uint32_t area = 0;
    int n = 0;
    const pixman_box32_t* r = pixman_region32_rectangles(region, &n);
    for (int i = 0; i < n; ++i)
        area += uint32_t(r[i].x2 - r[i].x1) * uint32_t(r[i].y2 - r[i].y1);
    return area;
}

void add_corner_region(pixman_region32_t* out, const Radii& c, int x, int y, int w, int h) {
    if (c.tl)
        pixman_region32_union_rect(out, out, x, y, unsigned(c.tl), unsigned(c.tl));
    if (c.tr)
        pixman_region32_union_rect(out, out, x + w - c.tr, y, unsigned(c.tr), unsigned(c.tr));
    if (c.bl)
        pixman_region32_union_rect(out, out, x, y + h - c.bl, unsigned(c.bl), unsigned(c.bl));
    if (c.br)
        pixman_region32_union_rect(out, out, x + w - c.br, y + h - c.br, unsigned(c.br), unsigned(c.br));
}

#ifdef ATRIUM_XWAYLAND
xwayland::XSurface* managed_xwayland_surface(Node* node) {
    if (node->type != Type::Buffer)
        return nullptr;
    SurfaceNode* s = static_cast<Buffer*>(node)->surface();
    if (!s)
        return nullptr;
    xwayland::XSurface* xs = xwayland::XSurface::from(s->surface);
    return xs && !xs->override_redirect ? xs : nullptr;
}
#endif

struct UpdateData {
    pixman_region32_t* visible;
    const pixman_region32_t* update_region;
    Box update_box;
    wl_list* outputs;
    bool calculate_visibility;
    bool restack_xwayland_surfaces;
#ifdef ATRIUM_XWAYLAND
    xwayland::XSurface* restack_above = nullptr;
#endif
};

bool update_iterator(Node* node, const Walk& w, UpdateData* data) {
    if (node->type == Type::BlurCache && static_cast<BlurCache*>(node)->dirty) {
        // Everything below it shows again (unculled), to be blurred afresh.
        pixman_region32_clear(data->visible);
        pixman_region32_copy(data->visible, data->update_region);
    }
    const Box box = box_of(node, w);
    pixman_region32_subtract(&node->visible, &node->visible, data->update_region);
    pixman_region32_union(&node->visible, &node->visible, data->visible);
    pixman_region32_intersect_rect(&node->visible, &node->visible, box.x, box.y, unsigned(box.width),
                                   unsigned(box.height));
    if (data->calculate_visibility) {
        pixman_region32_t opaque;
        pixman_region32_init(&opaque);
        SceneImpl::opaque_region(node, w, &opaque);
        pixman_region32_subtract(data->visible, data->visible, &opaque);
        pixman_region32_fini(&opaque);
    }
    SceneImpl::update_outputs(node, data->outputs, nullptr, nullptr);
#ifdef ATRIUM_XWAYLAND
    if (data->restack_xwayland_surfaces) {
        if (xwayland::XSurface* xs = managed_xwayland_surface(node)) {
            // Only when the whole node is being looked at.
            if (box_contains_box(&data->update_box, &box)) {
                if (data->restack_above)
                    xs->restack(data->restack_above, XCB_STACK_MODE_BELOW);
                else
                    xs->restack(nullptr, XCB_STACK_MODE_ABOVE);
            }
            data->restack_above = xs;
        }
    }
#endif
    return false;
}

void visibility(Node* node, pixman_region32_t* out) {
    if (!node->enabled)
        return;
    if (node->type == Type::Tree) {
        for (Node* child : each_child(static_cast<Tree*>(node)))
            visibility(child, out);
        return;
    }
    pixman_region32_union(out, out, &node->visible);
}

void bounds(Node* node, const Walk& w, pixman_region32_t* out) {
    if (!node->enabled)
        return;
    if (node->type == Type::Tree) {
        Tree* tree = static_cast<Tree*>(node);
        for (Node* child : each_child(tree))
            bounds(child, w.child(tree, child), out);
        return;
    }
    const Box b = box_of(node, w);
    pixman_region32_union_rect(out, out, b.x, b.y, unsigned(b.width), unsigned(b.height));
}

void cleanup_when_disabled(Node* node, bool restack, wl_list* outputs) {
    if (node->type == Type::Tree) {
        for (Node* child : each_child(static_cast<Tree*>(node)))
            if (child->enabled)
                cleanup_when_disabled(child, restack, outputs);
        return;
    }
    pixman_region32_clear(&node->visible);
    SceneImpl::update_outputs(node, outputs, nullptr, nullptr);
#ifdef ATRIUM_XWAYLAND
    if (restack)
        if (xwayland::XSurface* xs = managed_xwayland_surface(node))
            xs->restack(nullptr, XCB_STACK_MODE_BELOW);
#else
    (void)restack;
#endif
}

} // namespace

// ---- walking ------------------------------------------------------------

FBox fbox_of(const Node* node, const Walk& w) {
    int width = 0, height = 0;
    node->size(&width, &height);
    return {w.x, w.y, width * w.scale, height * w.scale};
}

Box box_of(const Node* node, const Walk& w) {
    const FBox f = fbox_of(node, w);
    if (w.scale == 1.0 && f.x == std::floor(f.x) && f.y == std::floor(f.y))
        return {int(f.x), int(f.y), int(f.width), int(f.height)};
    const int x1 = int(std::floor(f.x)), y1 = int(std::floor(f.y));
    return {x1, y1, int(std::ceil(f.x + f.width)) - x1, int(std::ceil(f.y + f.height)) - y1};
}

namespace {

bool nodes_in_box_at(Node* node, const Box& box, const BoxIterator& fn, const Walk& w) {
    if (!node->enabled)
        return false;
    if (node->type == Type::Tree) {
        Tree* tree = static_cast<Tree*>(node);
        for (Node* child : each_child_top_down(tree))
            if (nodes_in_box_at(child, box, fn, w.child(tree, child)))
                return true;
        return false;
    }
    Box nb = box_of(node, w);
    if (box_intersection(&nb, &nb, &box) && fn(node, w))
        return true;
    return false;
}

} // namespace

Walk walk_of(const Node* node) {
    std::vector<const Node*> chain;
    for (const Node* n = node; n; n = n->parent)
        chain.push_back(n);
    // The root's own offset counts, unscaled.
    Walk w{double(chain.back()->x), double(chain.back()->y), 1, 1};
    for (size_t i = chain.size() - 1; i > 0; --i)
        w = w.child(static_cast<const Tree*>(chain[i]), chain[i - 1]);
    return w;
}

bool nodes_in_box(Node* node, const Box& box, const BoxIterator& fn) {
    return nodes_in_box_at(node, box, fn, walk_of(node));
}

void scale_region(pixman_region32_t* region, float scale, bool round_up) {
    region_scale(region, region, scale);
    if (round_up && std::floor(scale) != scale)
        region_expand(region, region, 1);
}

int scale_length(int length, int offset, float scale) {
    return int(std::round((offset + length) * scale) - std::round(offset * scale));
}

void scale_box(Box* box, float scale) {
    box->width = scale_length(box->width, box->x, scale);
    box->height = scale_length(box->height, box->y, scale);
    box->x = int(std::round(box->x * scale));
    box->y = int(std::round(box->y * scale));
}

// ---- SceneImpl ------------------------------------------------------------

void SceneImpl::opaque_region(Node* node, const Walk& w, pixman_region32_t* opaque) {
    // Scaled or faded, nothing is sure to cover whole pixels.
    if (!w.identity())
        return;
    const int x = int(w.x), y = int(w.y);
    int width = 0, height = 0;
    node->size(&width, &height);
    switch (node->type) {
    case Type::Rect: {
        Rect* r = static_cast<Rect*>(node);
        if (r->color[3] != 1)
            return;
        pixman_region32_fini(opaque);
        pixman_region32_init_rect(opaque, x, y, unsigned(width), unsigned(height));
        if (!r->corners.empty()) {
            pixman_region32_t c;
            pixman_region32_init(&c);
            add_corner_region(&c, r->corners, x, y, width, height);
            pixman_region32_subtract(opaque, opaque, &c);
            pixman_region32_fini(&c);
        }
        if (!r->cut.empty()) {
            pixman_region32_t c;
            pixman_region32_init_rect(&c, r->cut.area.x + x, r->cut.area.y + y, unsigned(r->cut.area.width),
                                      unsigned(r->cut.area.height));
            pixman_region32_subtract(opaque, opaque, &c);
            pixman_region32_fini(&c);
        }
        return;
    }
    case Type::Buffer: {
        Buffer* b = static_cast<Buffer*>(node);
        if (!b->buffer || b->opacity != 1)
            return;
        if (!b->buffer_is_opaque_) {
            pixman_region32_copy(opaque, &b->opaque_region);
            pixman_region32_intersect_rect(opaque, opaque, 0, 0, unsigned(width), unsigned(height));
            pixman_region32_translate(opaque, x, y);
        } else {
            pixman_region32_fini(opaque);
            pixman_region32_init_rect(opaque, x, y, unsigned(width), unsigned(height));
        }
        if (!b->corners.empty()) {
            pixman_region32_t c;
            pixman_region32_init(&c);
            add_corner_region(&c, b->corners, x, y, width, height);
            pixman_region32_subtract(opaque, opaque, &c);
            pixman_region32_fini(&c);
        }
        return;
    }
    default:
        return;  // shadows and blur: always see-through
    }
}

bool SceneImpl::invisible(Node* node) {
    switch (node->type) {
    case Type::Tree:
        return true;
    case Type::Rect:
        return static_cast<Rect*>(node)->color[3] == 0;
    case Type::Buffer: {
        Buffer* b = static_cast<Buffer*>(node);
        return !b->buffer && !b->texture_;
    }
    case Type::Shadow:
        return static_cast<Shadow*>(node)->color[3] == 0;
    default:
        return false;
    }
}

void SceneImpl::update_outputs(Node* node, wl_list* outputs, SceneOutput* ignore, SceneOutput* force) {
    if (node->type != Type::Buffer)
        return;
    Buffer* b = static_cast<Buffer*>(node);
    SceneOutput* old_primary = b->primary_output;
    b->primary_output = nullptr;
    size_t count = 0;
    uint64_t active = 0;
    uint32_t largest = 0;

    if (pixman_region32_not_empty(&node->visible)) {
        const uint32_t visible_area = region_area(&node->visible);
        SceneOutput* o;
        wl_list_for_each(o, outputs, link) {
            if (o == ignore || !o->output->enabled)
                continue;
            Box box{o->x, o->y, 0, 0};
            o->output->effective_resolution(&box.width, &box.height);
            pixman_region32_t isect;
            pixman_region32_init(&isect);
            pixman_region32_intersect_rect(&isect, &node->visible, box.x, box.y, unsigned(box.width),
                                           unsigned(box.height));
            const uint32_t overlap = region_area(&isect);
            pixman_region32_fini(&isect);
            // Less than a tenth of what shows: not on this output.
            if (overlap >= 0.1 * visible_area) {
                if (overlap >= largest) {
                    largest = overlap;
                    b->primary_output = o;
                }
                active |= 1ull << o->index;
                ++count;
            }
        }
    }
    if (old_primary != b->primary_output)
        b->feedback_sent = {};

    const uint64_t old_active = b->active_outputs_;
    b->active_outputs_ = active;
    SceneOutput* o;
    wl_list_for_each(o, outputs, link) {
        const uint64_t mask = 1ull << o->index;
        if ((active & mask) && !(old_active & mask))
            wl_signal_emit_mutable(&b->events.output_enter, o);
        else if (!(active & mask) && (old_active & mask))
            wl_signal_emit_mutable(&b->events.output_leave, o);
    }
    if (old_active == active && (!force || ((1ull << force->index) & ~active)) && old_primary == b->primary_output)
        return;

    SceneOutput* list[64];
    size_t i = 0;
    wl_list_for_each(o, outputs, link)
        if (active & (1ull << o->index))
            list[i++] = o;
    OutputsUpdateEvent ev{list, count};
    wl_signal_emit_mutable(&b->events.outputs_update, &ev);
}

void SceneImpl::output_update(Node* node, wl_list* outputs, SceneOutput* ignore, SceneOutput* force) {
    if (node->type == Type::Tree) {
        for (Node* child : each_child(static_cast<Tree*>(node)))
            output_update(child, outputs, ignore, force);
        return;
    }
    update_outputs(node, outputs, ignore, force);
}

void SceneImpl::update_region(Scene* scene, const pixman_region32_t* region) {
    pixman_region32_t visible;
    pixman_region32_init(&visible);
    pixman_region32_copy(&visible, region);
    const pixman_box32_t* e = pixman_region32_extents(region);
    UpdateData data{
        &visible, region, {e->x1, e->y1, e->x2 - e->x1, e->y2 - e->y1}, &scene->outputs,
        scene->calculate_visibility, scene->restack_xwayland_surfaces,
    };
    nodes_in_box(scene, data.update_box, [&](Node* n, const Walk& w) { return update_iterator(n, w, &data); });
    pixman_region32_fini(&visible);
}

void SceneImpl::damage_outputs(Scene* scene, const pixman_region32_t* damage) {
    if (!pixman_region32_not_empty(damage))
        return;
    SceneOutput* o;
    wl_list_for_each(o, &scene->outputs, link) {
        pixman_region32_t d;
        pixman_region32_init(&d);
        pixman_region32_copy(&d, damage);
        pixman_region32_translate(&d, -o->x, -o->y);
        scale_region(&d, o->output->scale, true);
        int w, h;
        o->output->transformed_resolution(&w, &h);
        region_transform(&d, &d, output_transform_invert(o->output->transform), w, h);
        o->damage(&d);
        pixman_region32_fini(&d);
    }
}

void SceneImpl::send_frame_done(Node* node, SceneOutput* out, const timespec* now) {
    if (!node->enabled)
        return;
    if (node->type == Type::Buffer) {
        FrameDoneEvent ev{out, *now};
        static_cast<Buffer*>(node)->send_frame_done(&ev);
    } else if (node->type == Type::Tree) {
        for (Node* child : each_child(static_cast<Tree*>(node)))
            send_frame_done(child, out, now);
    }
}

void SceneImpl::mark_cache_dirty(Node* node) {
    if (node->type == Type::BlurCache) {
        static_cast<BlurCache*>(node)->mark_dirty();
    } else if (node->type == Type::Tree) {
        for (Node* child : each_child(static_cast<Tree*>(node)))
            mark_cache_dirty(child);
    }
}

// ---- Node -----------------------------------------------------------------

Node::Node(Type t, Tree* p) : type(t), parent(p) {
    wl_list_init(&link);
    wl_signal_init(&events.destroy);
    pixman_region32_init(&visible);
    if (p)
        wl_list_insert(p->children.prev, &link);
    addon_set_init(&addons);
}

Node::~Node() {
    wl_list_remove(&link);
    pixman_region32_fini(&visible);
}

void Node::destroy() {
    // Destroy listeners first: they may take children away with them.
    wl_signal_emit_mutable(&events.destroy, nullptr);
    addon_set_finish(&addons);
    set_enabled(false);

    Scene* scene = root();
    if (type == Type::Buffer) {
        Buffer* b = static_cast<Buffer*>(this);
        if (b->active_outputs_) {
            SceneOutput* o;
            wl_list_for_each(o, &scene->outputs, link)
                if (b->active_outputs_ & (1ull << o->index))
                    wl_signal_emit_mutable(&b->events.output_leave, o);
        }
    } else if (type == Type::Tree) {
        Tree* tree = static_cast<Tree*>(this);
        if (tree == scene) {
            SceneOutput *o, *tmp;
            wl_list_for_each_safe(o, tmp, &scene->outputs, link)
                o->destroy();
        }
        for (Node* child : each_child(tree))
            child->destroy();
    }
    delete this;
}

Scene* Node::root() const {
    const Node* n = this;
    while (n->parent)
        n = n->parent;
    return static_cast<Scene*>(const_cast<Node*>(n));
}

bool Node::coords(int* lx, int* ly) const {
    const Placement p = placement();
    *lx = int(std::lround(p.x));
    *ly = int(std::lround(p.y));
    return p.enabled;
}

Placement Node::placement() const {
    const Walk w = walk_of(this);
    bool on = true;
    for (const Node* n = this; n; n = n->parent)
        on = on && n->enabled;
    return {w.x, w.y, w.scale, w.opacity, on};
}

void Node::size(int* w, int* h) const {
    *w = *h = 0;
    switch (type) {
    case Type::Tree:
        return;
    case Type::Rect:
        *w = static_cast<const Rect*>(this)->width;
        *h = static_cast<const Rect*>(this)->height;
        return;
    case Type::Shadow:
        *w = static_cast<const Shadow*>(this)->width;
        *h = static_cast<const Shadow*>(this)->height;
        return;
    case Type::Blur:
        *w = static_cast<const Blur*>(this)->width;
        *h = static_cast<const Blur*>(this)->height;
        return;
    case Type::BlurCache:
        *w = static_cast<const BlurCache*>(this)->width;
        *h = static_cast<const BlurCache*>(this)->height;
        return;
    case Type::Buffer: {
        const Buffer* b = static_cast<const Buffer*>(this);
        if (b->dst_width > 0 && b->dst_height > 0) {
            *w = b->dst_width;
            *h = b->dst_height;
        } else {
            *w = b->buffer_width_;
            *h = b->buffer_height_;
            output_transform_coords(b->transform, w, h);
        }
        return;
    }
    }
}

void Node::update(pixman_region32_t* damage) {
    Scene* scene = root();
    int lx, ly;
    if (!coords(&lx, &ly)) {
        // Explicit damage on a disabled tree: the node was just disabled.
        if (damage) {
            cleanup_when_disabled(this, scene->restack_xwayland_surfaces, &scene->outputs);
            SceneImpl::update_region(scene, damage);
            SceneImpl::damage_outputs(scene, damage);
            pixman_region32_fini(damage);
        }
        return;
    }
    pixman_region32_t own;
    if (!damage) {
        pixman_region32_init(&own);
        visibility(this, &own);
        damage = &own;
    }
    pixman_region32_t region;
    pixman_region32_init(&region);
    pixman_region32_copy(&region, damage);
    bounds(this, walk_of(this), &region);
    SceneImpl::update_region(scene, &region);
    pixman_region32_fini(&region);

    visibility(this, damage);
    SceneImpl::damage_outputs(scene, damage);
    pixman_region32_fini(damage);
}

void Node::set_enabled(bool on) {
    if (enabled == on)
        return;
    pixman_region32_t was;
    pixman_region32_init(&was);
    int lx, ly;
    if (coords(&lx, &ly))
        visibility(this, &was);
    enabled = on;
    update(&was);
}

void Node::set_position(int nx, int ny) {
    if (x == nx && y == ny)
        return;
    x = nx;
    y = ny;
    update();
}

void Node::place_above(Node* sibling) {
    assert(sibling != this && sibling->parent == parent);
    if (link.prev == &sibling->link)
        return;
    wl_list_remove(&link);
    wl_list_insert(&sibling->link, &link);
    update();
}

void Node::place_below(Node* sibling) {
    assert(sibling != this && sibling->parent == parent);
    if (link.next == &sibling->link)
        return;
    wl_list_remove(&link);
    wl_list_insert(sibling->link.prev, &link);
    update();
}

void Node::raise_to_top() {
    Node* top = node_of_link(parent->children.prev);
    if (top != this)
        place_above(top);
}

void Node::lower_to_bottom() {
    Node* bottom = node_of_link(parent->children.next);
    if (bottom != this)
        place_below(bottom);
}

void Node::reparent(Tree* p) {
    assert(p);
    if (parent == p)
        return;
    for (Node* a = p; a; a = a->parent)
        assert(a != this);  // never its own ancestor
    pixman_region32_t was;
    pixman_region32_init(&was);
    int lx, ly;
    if (coords(&lx, &ly))
        visibility(this, &was);
    wl_list_remove(&link);
    parent = p;
    wl_list_insert(p->children.prev, &link);
    update(&was);
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winvalid-offsetof"
Node* node_of_link(wl_list* link) {
    return reinterpret_cast<Node*>(reinterpret_cast<char*>(link) - offsetof(Node, link));
}
#pragma GCC diagnostic pop

namespace {

void for_each_buffer_at(Node* node, const Walk& w, const std::function<void(Buffer*, int, int)>& fn) {
    if (!node->enabled)
        return;
    if (node->type == Type::Buffer) {
        fn(static_cast<Buffer*>(node), int(std::lround(w.x)), int(std::lround(w.y)));
    } else if (node->type == Type::Tree) {
        Tree* tree = static_cast<Tree*>(node);
        for (Node* child : each_child(tree))
            for_each_buffer_at(child, w.child(tree, child), fn);
    }
}

} // namespace

void Node::for_each_buffer(const std::function<void(Buffer*, int, int)>& fn) {
    // Relative to this node, as wlroots does.
    Walk w{double(x), double(y), 1, 1};
    if (!enabled)
        return;
    if (type == Type::Buffer) {
        fn(static_cast<Buffer*>(this), x, y);
        return;
    }
    if (type == Type::Tree) {
        Tree* tree = static_cast<Tree*>(this);
        for (Node* child : each_child(tree))
            for_each_buffer_at(child, w.child(tree, child), fn);
    }
}

Node* Node::at(double lx, double ly, double* nx, double* ny) {
    const Box box{int(std::floor(lx)), int(std::floor(ly)), 1, 1};
    Node* found = nullptr;
    double rx = 0, ry = 0;
    nodes_in_box(this, box, [&](Node* node, const Walk& w) {
        double x = (lx - w.x) / w.scale, y = (ly - w.y) / w.scale;
        if (node->type == Type::Buffer) {
            Buffer* b = static_cast<Buffer*>(node);
            if (b->point_accepts_input && !b->point_accepts_input(b, &x, &y))
                return false;
        } else if (node->type == Type::Rect) {
            Rect* r = static_cast<Rect*>(node);
            if (!r->accepts_input)
                return false;
            if (!r->cut.empty() && box_contains_point(&r->cut.area, x, y))
                return false;
        } else {
            return false;  // shadows and blur take no input
        }
        rx = x;
        ry = y;
        found = node;
        return true;
    });
    if (found) {
        if (nx)
            *nx = rx;
        if (ny)
            *ny = ry;
    }
    return found;
}

// ---- Tree -------------------------------------------------------------------

Tree::Tree(Tree* parent, Type t) : Node(t, parent) {
    wl_list_init(&children);
}

Tree::~Tree() = default;

Tree* Tree::create(Tree* parent) {
    assert(parent);
    return new Tree(parent);
}

void Tree::set_scale(float s) {
    if (s == scale_ || s <= 0)
        return;
    pixman_region32_t was;
    pixman_region32_init(&was);
    int lx, ly;
    if (coords(&lx, &ly))
        visibility(this, &was);
    scale_ = s;
    update(&was);
}

void Tree::set_warp(std::function<std::pair<double, double>(double, double)> fn, FBox frame) {
    if (!fn && !warp_)
        return;
    pixman_region32_t was;
    pixman_region32_init(&was);
    int lx, ly;
    if (coords(&lx, &ly))
        visibility(this, &was);
    warp_ = std::move(fn);
    warp_frame_ = frame;
    update(&was);
    // Where a warp lands isn't its nodes' boxes: every step repaints the
    // screens it may be on.
    if (Scene* s = root()) {
        pixman_region32_t all;
        pixman_region32_init_rect(&all, -(1 << 20), -(1 << 20), 1u << 21, 1u << 21);
        SceneImpl::damage_outputs(s, &all);
        pixman_region32_fini(&all);
    }
}

void Tree::set_opacity(float o) {
    o = std::clamp(o, 0.0f, 1.0f);
    if (o == opacity_)
        return;
    opacity_ = o;
    update();
}

// ---- Rect -------------------------------------------------------------------

Rect::Rect(Tree* parent, int w, int h, const float c[4]) : Node(Type::Rect, parent), width(w), height(h) {
    std::memcpy(color, c, sizeof(color));
}

Rect* Rect::create(Tree* parent, int w, int h, const float c[4]) {
    assert(parent && w >= 0 && h >= 0);
    Rect* r = new Rect(parent, w, h, c);
    r->update();
    return r;
}

void Rect::set_size(int w, int h) {
    if (width == w && height == h)
        return;
    width = w;
    height = h;
    update();
}

void Rect::set_color(const float c[4]) {
    if (std::memcmp(color, c, sizeof(color)) == 0)
        return;
    std::memcpy(color, c, sizeof(color));
    update();
}

void Rect::set_corner_radii(Radii r) {
    if (corners == r)
        return;
    corners = r;
    update();
}

void Rect::set_cut_out(const CutOut& c) {
    if (cut == c)
        return;
    cut = c;
    update();
}

// ---- Shadow -----------------------------------------------------------------

Shadow::Shadow(Tree* parent, int w, int h, int r, float s, const float c[4])
    : Node(Type::Shadow, parent), width(w), height(h), corner_radius(r), blur_sigma(s) {
    std::memcpy(color, c, sizeof(color));
}

Shadow* Shadow::create(Tree* parent, int w, int h, int r, float s, const float c[4]) {
    assert(parent);
    Shadow* sh = new Shadow(parent, w, h, r, s, c);
    sh->update();
    return sh;
}

void Shadow::set_size(int w, int h) {
    if (width == w && height == h)
        return;
    width = w;
    height = h;
    update();
}

void Shadow::set_corner_radius(int r) {
    if (corner_radius == r)
        return;
    corner_radius = r;
    update();
}

void Shadow::set_blur_sigma(float s) {
    if (blur_sigma == s)
        return;
    blur_sigma = s;
    update();
}

void Shadow::set_color(const float c[4]) {
    if (std::memcmp(color, c, sizeof(color)) == 0)
        return;
    std::memcpy(color, c, sizeof(color));
    update();
}

void Shadow::set_cut_out(const CutOut& c) {
    if (cut == c)
        return;
    cut = c;
    update();
}

// ---- Blur -------------------------------------------------------------------

Blur::Blur(Tree* parent, int w, int h) : Node(Type::Blur, parent), width(w), height(h) {}

Blur::~Blur() {
    if (mask_)
        mask_->mask_of_ = nullptr;
}

Blur* Blur::create(Tree* parent, int w, int h) {
    assert(parent);
    Blur* b = new Blur(parent, w, h);
    b->update();
    return b;
}

void Blur::set_size(int w, int h) {
    if (width == w && height == h)
        return;
    width = w;
    height = h;
    update();
}

void Blur::set_corner_radii(Radii r) {
    if (corners == r)
        return;
    corners = r;
    update();
}

void Blur::set_use_cache(bool c) {
    if (use_cache == c)
        return;
    use_cache = c;
    update();
}

void Blur::set_mask(Buffer* m) {
    if (mask_ == m)
        return;
    if (mask_)
        mask_->mask_of_ = nullptr;
    if (m && m->mask_of_)
        m->mask_of_->mask_ = nullptr;
    mask_ = m;
    if (m)
        m->mask_of_ = this;
    update();
}

void Blur::set_alpha(float a) {
    if (alpha == a)
        return;
    alpha = a;
    update();
}

void Blur::set_strength(float s) {
    if (strength == s)
        return;
    strength = s;
    update();
}

void Blur::set_refraction(float r, float t) {
    if (refraction == r && thickness == t)
        return;
    refraction = r;
    thickness = t;
    update();
}

void Blur::set_glass(const GlassMaterial& g) {
    if (glass == g)
        return;
    glass = g;
    update();
}

void Blur::set_glass_shapes(const GlassShape* s, int count) {
    count = std::clamp(count, 0, kMaxGlassShapes);
    if (count == shape_count && std::equal(s, s + count, shapes))
        return;
    std::copy(s, s + count, shapes);
    shape_count = count;
    update();
}

void Blur::set_cut_out(const CutOut& c) {
    if (cut == c)
        return;
    cut = c;
    update();
}

// ---- BlurCache ----------------------------------------------------------------

BlurCache::BlurCache(Tree* parent, int w, int h) : Node(Type::BlurCache, parent), width(w), height(h) {}

BlurCache* BlurCache::create(Tree* parent, int w, int h) {
    assert(parent);
    BlurCache* c = new BlurCache(parent, w, h);
    c->update();
    return c;
}

void BlurCache::set_size(int w, int h) {
    if (width == w && height == h)
        return;
    width = w;
    height = h;
    mark_dirty();
}

void BlurCache::mark_dirty() {
    // A disabled cache isn't drawn: marking it would only keep what's
    // under it from being culled.
    if (!enabled)
        return;
    dirty = true;
    update();
}

// ---- Buffer -------------------------------------------------------------------

Buffer::Buffer(Tree* parent, atrium::Buffer* b) : Node(Type::Buffer, parent) {
    wl_signal_init(&events.outputs_update);
    wl_signal_init(&events.output_enter);
    wl_signal_init(&events.output_leave);
    wl_signal_init(&events.output_sample);
    wl_signal_init(&events.frame_done);
    pixman_region32_init(&opaque_region);
    note_single_pixel(b);  // as set_buffer does: a copy of one is a rectangle too
    take_buffer(b);
}

Buffer::~Buffer() {
    delete surface_;
    take_buffer(nullptr);
    set_texture(nullptr);
    pixman_region32_fini(&opaque_region);
    if (wait_timeline_)
        timeline_unref(wait_timeline_);
    if (mask_of_)
        mask_of_->mask_ = nullptr;
}

Buffer* Buffer::create(Tree* parent, atrium::Buffer* b) {
    assert(parent);
    Buffer* n = new Buffer(parent, b);
    n->update();
    return n;
}

void Buffer::take_buffer(atrium::Buffer* b) {
    buffer_release_.disconnect();
    if (own_buffer_ && buffer)
        buffer_unlock(buffer);
    buffer = nullptr;
    own_buffer_ = false;
    buffer_width_ = buffer_height_ = 0;
    buffer_is_opaque_ = false;
    if (!b)
        return;
    own_buffer_ = true;
    buffer = buffer_lock(b);
    buffer_width_ = b->width;
    buffer_height_ = b->height;
    buffer_is_opaque_ = buffer_is_opaque(b);
    buffer_release_.connect(&b->events.release, [this](void*) {
        buffer = nullptr;
        buffer_release_.disconnect();
    });
}

void Buffer::set_texture(render::Texture* t) {
    renderer_destroy_.disconnect();
    if (texture_)
        texture_->destroy();
    texture_ = t;
    if (t)
        renderer_destroy_.connect(&t->renderer->events.destroy, [this](void*) { set_texture(nullptr); });
}

render::Texture* Buffer::texture(render::Renderer* renderer) {
    if (!buffer || texture_)
        return texture_;
    if (wl::SurfaceBuffer* sb = wl::SurfaceBuffer::from(buffer))
        return sb->texture;
    render::Texture* t = renderer->texture_from_buffer(buffer);
    if (t && own_buffer_) {
        own_buffer_ = false;
        buffer_unlock(buffer);
    }
    set_texture(t);
    return t;
}

// Single-pixel buffers are drawn as rectangles: remember the colour, the
// buffer may be gone after the upload.
void Buffer::note_single_pixel(atrium::Buffer* b) {
    single_pixel_ = false;
    wl::SurfaceBuffer* sb = b ? wl::SurfaceBuffer::from(b) : nullptr;
    if (sb && sb->source.get())
        single_pixel_ = wl::SinglePixelBuffers::color_of(sb->source.get(), single_pixel_color_);
}

bool Buffer::is_black_opaque() const {
    return single_pixel_ && single_pixel_color_[0] == 0 && single_pixel_color_[1] == 0 &&
           single_pixel_color_[2] == 0 && single_pixel_color_[3] == 1 && opacity == 1 && corners.empty();
}

void Buffer::set_buffer(atrium::Buffer* b, const BufferOptions& o) {
    assert(b || !o.damage);
    const bool mapped = b != nullptr;
    const bool was_mapped = buffer || texture_;
    if (!mapped && !was_mapped)
        return;

    bool changed = mapped != was_mapped;
    if (b && dst_width == 0 && dst_height == 0)
        changed = changed || buffer_width_ != b->width || buffer_height_ != b->height;

    if (b != buffer)
        note_single_pixel(b);

    take_buffer(b);
    set_texture(nullptr);
    if (wait_timeline_)
        timeline_unref(wait_timeline_);
    wait_timeline_ = o.wait_timeline ? timeline_ref(o.wait_timeline) : nullptr;
    wait_point_ = o.wait_timeline ? o.wait_point : 0;

    if (changed) {
        update();  // damages all of it
        return;
    }
    if (!b)
        return;
    const Placement place = placement();
    if (!place.enabled)
        return;

    pixman_region32_t fallback;
    pixman_region32_init_rect(&fallback, 0, 0, unsigned(b->width), unsigned(b->height));
    const pixman_region32_t* damage = o.damage ? o.damage : &fallback;

    FBox box = src_box;
    if (fbox_empty(&box))
        box = {0, 0, double(b->width), double(b->height)};
    fbox_transform(&box, &box, transform, b->width, b->height);
    double sx, sy;
    if (dst_width || dst_height) {
        sx = dst_width / box.width;
        sy = dst_height / box.height;
    } else {
        sx = b->width / box.width;
        sy = b->height / box.height;
    }
    sx *= place.scale;
    sy *= place.scale;

    pixman_region32_t trans;
    pixman_region32_init(&trans);
    region_transform(&trans, damage, transform, b->width, b->height);
    pixman_region32_intersect_rect(&trans, &trans, int(box.x), int(box.y), unsigned(box.width), unsigned(box.height));
    pixman_region32_translate(&trans, -int(box.x), -int(box.y));

    Scene* scene = root();
    SceneOutput* o2;
    wl_list_for_each(o2, &scene->outputs, link) {
        const float os = o2->output->scale;
        const float osx = float(os * sx), osy = float(os * sy);
        pixman_region32_t od;
        pixman_region32_init(&od);
        region_scale_xy(&od, &trans, osx, osy);
        // Linear filtering bleeds scaled content into neighbouring pixels.
        const float bsx = 1.0f / osx, bsy = 1.0f / osy;
        const int dx = std::floor(bsx) != bsx ? int(std::ceil(osx / 2.0f)) : 0;
        const int dy = std::floor(bsy) != bsy ? int(std::ceil(osy / 2.0f)) : 0;
        region_expand(&od, &od, std::max(dx, dy));
        // Only where it shows.
        pixman_region32_t cull;
        pixman_region32_init(&cull);
        pixman_region32_copy(&cull, &visible);
        scale_region(&cull, os, true);
        pixman_region32_translate(&cull, -int(std::lround(place.x * os)), -int(std::lround(place.y * os)));
        pixman_region32_intersect(&od, &od, &cull);
        pixman_region32_fini(&cull);

        pixman_region32_translate(&od, int(std::lround((place.x - o2->x) * os)),
                                  int(std::lround((place.y - o2->y) * os)));
        int w, h;
        o2->output->transformed_resolution(&w, &h);
        region_transform(&od, &od, output_transform_invert(o2->output->transform), w, h);
        o2->damage(&od);
        pixman_region32_fini(&od);
    }
    pixman_region32_fini(&trans);
    pixman_region32_fini(&fallback);
}

void Buffer::set_opaque_region(const pixman_region32_t* region) {
    if (pixman_region32_equal(&opaque_region, region))
        return;
    pixman_region32_copy(&opaque_region, region);
    if (!placement().enabled)
        return;
    pixman_region32_t r;
    pixman_region32_init(&r);
    bounds(this, walk_of(this), &r);
    SceneImpl::update_region(root(), &r);
    pixman_region32_fini(&r);
}

void Buffer::set_source_box(const FBox* box) {
    const FBox b = box ? *box : FBox{};
    if (fbox_equal(&src_box, &b))
        return;
    src_box = b;
    update();
}

void Buffer::set_dest_size(int w, int h) {
    if (dst_width == w && dst_height == h)
        return;
    dst_width = w;
    dst_height = h;
    update();
}

void Buffer::set_transform(wl_output_transform t) {
    if (transform == t)
        return;
    transform = t;
    update();
}

void Buffer::set_opacity(float o) {
    if (opacity == o)
        return;
    opacity = std::clamp(o, 0.0f, 1.0f);
    update();
}

void Buffer::set_filter_mode(render::ScaleFilter m) {
    if (filter_mode == m)
        return;
    filter_mode = m;
    update();
}

void Buffer::set_transfer_function(TransferFunction tf) {
    if (transfer_function == tf)
        return;
    transfer_function = tf;
    update();
}

void Buffer::set_primaries(NamedPrimaries p) {
    if (primaries == p)
        return;
    primaries = p;
    update();
}

void Buffer::set_corner_radii(Radii r) {
    if (corners == r)
        return;
    corners = r;
    update();
}

void Buffer::send_frame_done(FrameDoneEvent* ev) {
    if (pixman_region32_not_empty(&visible))
        wl_signal_emit_mutable(&events.frame_done, ev);
}

// ---- Scene --------------------------------------------------------------------

Scene::Scene() : Tree(nullptr) {
    wl_list_init(&outputs);
    const char* debug = std::getenv("WLR_SCENE_DEBUG_DAMAGE");
    if (debug && std::strcmp(debug, "rerender") == 0)
        debug_damage = DebugDamage::Rerender;
    else if (debug && std::strcmp(debug, "highlight") == 0)
        debug_damage = DebugDamage::Highlight;
    direct_scanout = !env_true("WLR_SCENE_DISABLE_DIRECT_SCANOUT");
    calculate_visibility = !env_true("WLR_SCENE_DISABLE_VISIBILITY");
}

Scene::~Scene() = default;

Scene* Scene::create() { return new Scene(); }

void Scene::set_blur(const render::BlurParams& p) {
    if (p.passes == blur_.passes && p.radius == blur_.radius && p.noise == blur_.noise &&
        p.brightness == blur_.brightness && p.contrast == blur_.contrast && p.saturation == blur_.saturation)
        return;
    blur_ = p;
    SceneImpl::mark_cache_dirty(this);
    update();
}

void Scene::set_gamma_controls(wl::GammaControls* g) {
    gamma_ = g;
    gamma_set_.disconnect();
    SceneOutput* o;
    wl_list_for_each(o, &outputs, link) {
        o->gamma_lut_changed_ = false;
        color_transform_unref(o->gamma_lut_transform_);
        o->gamma_lut_transform_ = nullptr;
    }
    if (!g)
        return;
    gamma_set_ = g->set_gamma.connect([this](wl::Output* out, const std::vector<uint16_t>& table) {
        SceneOutput* o = output_for(out);
        if (!o)
            return;
        o->gamma_lut_changed_ = true;
        color_transform_unref(o->gamma_lut_transform_);
        o->gamma_lut_transform_ = nullptr;
        if (!table.empty()) {
            const size_t n = table.size() / 3;
            o->gamma_lut_transform_ =
                color_transform_init_lut_3x1d(n, table.data(), table.data() + n, table.data() + 2 * n);
        }
        o->output->schedule_frame();
    });
}

} // namespace atrium::scene
