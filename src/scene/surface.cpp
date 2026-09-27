// Surfaces in the scene: ported from wlroots' types/scene (surface.c,
// subsurface_tree.c, xdg_shell.c, layer_shell_v1.c, drag_icon.c; MIT).

#include "scene/internal.hpp"

#include <algorithm>
#include <cassert>

extern "C" {
#include <wlr/types/wlr_color_representation_v1.h>
}

namespace atrium::scene {

namespace {

double preferred_buffer_scale(wlr_surface* surface) {
    double scale = 1;
    wlr_surface_output* so;
    wl_list_for_each(so, &surface->current_outputs, link)
        scale = std::max(scale, double(so->output->scale));
    return scale;
}

// The output that paces the surface's frames (callbacks, presentation).
wlr_output* frame_pacing_output(wlr_surface* surface) {
    wlr_output* best = nullptr;
    wlr_surface_output* so;
    wl_list_for_each(so, &surface->current_outputs, link)
        if (!so->WLR_PRIVATE.suspended && (!best || so->output->refresh > best->refresh))
            best = so->output;
    return best;
}

int tf_preference(wlr_color_transfer_function tf) {
    switch (tf) {
    case WLR_COLOR_TRANSFER_FUNCTION_GAMMA22:
        return 0;
    case WLR_COLOR_TRANSFER_FUNCTION_ST2084_PQ:
        return 1;
    default:
        return -1;
    }
}

int primaries_preference(wlr_color_named_primaries p) {
    return p == WLR_COLOR_NAMED_PRIMARIES_BT2020 ? 1 : 0;
}

wlr_image_description_v1_data preferred_image_description(wlr_surface* surface) {
    wlr_output_image_description preferred{};
    preferred.transfer_function = WLR_COLOR_TRANSFER_FUNCTION_GAMMA22;
    preferred.primaries = WLR_COLOR_NAMED_PRIMARIES_SRGB;
    wlr_surface_output* so;
    wl_list_for_each(so, &surface->current_outputs, link) {
        const wlr_output_image_description* d = so->output->image_description;
        if (!d)
            continue;
        if (tf_preference(preferred.transfer_function) < tf_preference(d->transfer_function))
            preferred.transfer_function = d->transfer_function;
        if (primaries_preference(preferred.primaries) < primaries_preference(d->primaries))
            preferred.primaries = d->primaries;
    }
    wlr_image_description_v1_data out{};
    out.tf_named = wlr_color_manager_v1_transfer_function_from_wlr(preferred.transfer_function);
    out.primaries_named = wlr_color_manager_v1_primaries_from_wlr(preferred.primaries);
    return out;
}

void unmark_client_buffer(Buffer* b) {
    if (!b->buffer)
        return;
    wlr_client_buffer* cb = wlr_client_buffer_get(b->buffer);
    if (cb && cb->WLR_PRIVATE.n_ignore_locks > 0)
        --cb->WLR_PRIVATE.n_ignore_locks;
}

bool surface_accepts_input(Buffer* b, double* sx, double* sy) {
    SurfaceNode* s = b->surface();
    *sx += s->clip.x;
    *sy += s->clip.y;
    return wlr_surface_point_accepts_input(s->surface, *sx, *sy);
}

} // namespace

// ---- SurfaceNode ------------------------------------------------------------

SurfaceNode::SurfaceNode(Buffer* b, wlr_surface* s) : buffer(b), surface(s) {}

SurfaceNode::~SurfaceNode() { unmark_client_buffer(buffer); }

SurfaceNode* SurfaceNode::create(Tree* parent, wlr_surface* surface) {
    Buffer* b = Buffer::create(parent, nullptr);
    auto* sn = new SurfaceNode(b, surface);
    b->surface_ = sn;
    b->point_accepts_input = surface_accepts_input;

    sn->outputs_update_.connect(&b->events.outputs_update, [sn](OutputsUpdateEvent* e) {
        Scene* scene = sn->buffer->root();
        // Seen on no output: keep what was last sent, so a surface hidden
        // and shown again on the same output isn't told to leave and enter.
        const bool suspend = e->size == 0;
        wlr_surface_output *entered, *tmp;
        wl_list_for_each_safe(entered, tmp, &sn->surface->current_outputs, link) {
            bool active = false;
            for (size_t i = 0; i < e->size; ++i)
                active = active || entered->output == e->active[i]->output;
            SceneOutput* o;
            wl_list_for_each(o, &scene->outputs, link) {
                if (o->output == entered->output) {
                    entered->WLR_PRIVATE.suspended = suspend;
                    if (!suspend && !active)
                        wlr_surface_send_leave(sn->surface, entered->output);
                    break;
                }
            }
        }
        if (suspend)
            return;
        for (size_t i = 0; i < e->size; ++i)
            wlr_surface_send_enter(sn->surface, e->active[i]->output);
        const double scale = preferred_buffer_scale(sn->surface);
        wlr_fractional_scale_v1_notify_scale(sn->surface, scale);
        wlr_surface_set_preferred_buffer_scale(sn->surface, int32_t(std::ceil(scale)));
        if (scene->color_manager_v1) {
            const wlr_image_description_v1_data d = preferred_image_description(sn->surface);
            wlr_color_manager_v1_set_surface_preferred_image_description(scene->color_manager_v1, sn->surface, &d);
        }
    });
    sn->output_sample_.connect(&b->events.output_sample, [sn](OutputSampleEvent* e) {
        wlr_output* output = e->output->output;
        if (frame_pacing_output(sn->surface) != output)
            return;
        if (e->direct_scanout)
            wlr_presentation_surface_scanned_out_on_output(sn->surface, output);
        else
            wlr_presentation_surface_textured_on_output(sn->surface, output);
        wlr_linux_drm_syncobj_surface_v1_state* sync = wlr_linux_drm_syncobj_v1_get_surface_state(sn->surface);
        if (sync && e->release_timeline)
            wlr_linux_drm_syncobj_v1_state_add_release_point(sync, e->release_timeline, e->release_point,
                                                             output->event_loop);
    });
    sn->frame_done_.connect(&b->events.frame_done, [sn](FrameDoneEvent* e) {
        if (frame_pacing_output(sn->surface) == e->output->output)
            wlr_surface_send_frame_done(sn->surface, &e->when);
    });
    sn->surface_destroy_.connect(&surface->events.destroy, [sn](void*) { sn->buffer->destroy(); });
    sn->surface_commit_.connect(&surface->events.commit, [sn](void*) {
        sn->reconfigure();
        // A frame asked for: schedule one where it would be seen.
        int lx, ly;
        const bool on = sn->buffer->coords(&lx, &ly);
        wlr_output* out = frame_pacing_output(sn->surface);
        if (!wl_list_empty(&sn->surface->current.frame_callback_list) && out && on)
            wlr_output_schedule_frame(out);
    });
    sn->reconfigure();
    return sn;
}

void SurfaceNode::send_frame_done(const timespec* when) {
    if (pixman_region32_not_empty(&buffer->visible))
        wlr_surface_send_frame_done(surface, when);
}

void SurfaceNode::set_clip(const wlr_box* c) {
    const wlr_box next = c ? *c : wlr_box{};
    if (wlr_box_equal(&next, &clip))
        return;
    clip = next;
    reconfigure();
}

void SurfaceNode::reconfigure() {
    Buffer* b = buffer;
    wlr_surface_state* state = &surface->current;
    wlr_fbox src;
    wlr_surface_get_buffer_source_box(surface, &src);
    pixman_region32_t opaque;
    pixman_region32_init(&opaque);
    pixman_region32_copy(&opaque, &surface->opaque_region);
    int width = state->width, height = state->height;

    if (!wlr_box_empty(&clip)) {
        int bw = state->buffer_width, bh = state->buffer_height;
        width = std::min(clip.width, width - clip.x);
        height = std::min(clip.height, height - clip.y);
        wlr_fbox_transform(&src, &src, state->transform, bw, bh);
        wlr_output_transform_coords(state->transform, &bw, &bh);
        src.x += double(clip.x) * src.width / state->width;
        src.y += double(clip.y) * src.height / state->height;
        src.width *= double(width) / state->width;
        src.height *= double(height) / state->height;
        wlr_fbox_transform(&src, &src, wlr_output_transform_invert(state->transform), bw, bh);
        pixman_region32_translate(&opaque, -clip.x, -clip.y);
        pixman_region32_intersect_rect(&opaque, &opaque, 0, 0, unsigned(std::max(0, width)),
                                       unsigned(std::max(0, height)));
    }
    if (width <= 0 || height <= 0) {
        b->set_buffer(nullptr);
        pixman_region32_fini(&opaque);
        return;
    }

    float opacity = 1;
    if (const wlr_alpha_modifier_surface_v1_state* a = wlr_alpha_modifier_v1_get_surface_state(surface))
        opacity = float(a->multiplier);

    wlr_color_transfer_function tf = WLR_COLOR_TRANSFER_FUNCTION_GAMMA22;
    wlr_color_named_primaries primaries = WLR_COLOR_NAMED_PRIMARIES_SRGB;
    if (const wlr_image_description_v1_data* d = wlr_surface_get_image_description_v1_data(surface)) {
        tf = wlr_color_manager_v1_transfer_function_to_wlr(wp_color_manager_v1_transfer_function(d->tf_named));
        primaries = wlr_color_manager_v1_primaries_to_wlr(wp_color_manager_v1_primaries(d->primaries_named));
    }
    wlr_color_encoding encoding = WLR_COLOR_ENCODING_NONE;
    wlr_color_range range = WLR_COLOR_RANGE_NONE;
    if (const wlr_color_representation_v1_surface_state* r = wlr_color_representation_v1_get_surface_state(surface)) {
        if (r->coefficients)
            encoding = wlr_color_representation_v1_color_encoding_to_wlr(wp_color_representation_surface_v1_coefficients(r->coefficients));
        if (r->range)
            range = wlr_color_representation_v1_color_range_to_wlr(wp_color_representation_surface_v1_range(r->range));
    }

    b->set_opaque_region(&opaque);
    b->set_source_box(&src);
    b->set_dest_size(width, height);
    b->set_transform(wl_output_transform(state->transform));
    b->set_opacity(opacity);
    b->set_transfer_function(tf);
    b->set_primaries(primaries);
    b->set_color_encoding(encoding);
    b->set_color_range(range);
    unmark_client_buffer(b);

    if (surface->buffer) {
        // In-place texture updates are fine unless the colour of a
        // single-pixel buffer was cached.
        bool single_pixel = surface->buffer->source && wlr_single_pixel_buffer_v1_try_from_buffer(surface->buffer->source);
        if (!single_pixel)
            ++surface->buffer->WLR_PRIVATE.n_ignore_locks;
        BufferOptions o;
        o.damage = &surface->buffer_damage;
        if (wlr_linux_drm_syncobj_surface_v1_state* sync = wlr_linux_drm_syncobj_v1_get_surface_state(surface)) {
            o.wait_timeline = sync->acquire_timeline;
            o.wait_point = sync->acquire_point;
        }
        b->set_buffer(&surface->buffer->base, o);
    } else {
        b->set_buffer(nullptr);
    }
    pixman_region32_fini(&opaque);
}

// ---- subsurface trees -------------------------------------------------------

namespace {

struct SubsurfaceTree;

// A wlr_addon that knows its tree (SubsurfaceTree isn't standard layout).
struct AddonHook {
    wlr_addon addon{};
    SubsurfaceTree* self = nullptr;
};

struct SubsurfaceTree {
    Tree* tree = nullptr;
    wlr_surface* surface = nullptr;
    SurfaceNode* node = nullptr;
    SubsurfaceTree* parent = nullptr;  // null for the top surface
    wlr_box clip{};
    AddonHook scene_addon;    // on tree's node
    AddonHook surface_addon;  // sub-surfaces: on the surface, keyed by parent
    Listener<> surface_destroy, surface_commit, surface_map, surface_unmap, subsurface_destroy;
    Listener<wlr_subsurface> new_subsurface;
};

void tree_addon_destroy(wlr_addon* a);
void surface_addon_destroy(wlr_addon* a);
const wlr_addon_interface kTreeAddon = {.name = "atrium_subsurface_tree", .destroy = tree_addon_destroy};
const wlr_addon_interface kSurfaceAddon = {.name = "atrium_subsurface_tree", .destroy = surface_addon_destroy};

SubsurfaceTree* from_scene_addon(wlr_addon* a) { return reinterpret_cast<AddonHook*>(a)->self; }

SubsurfaceTree* from_surface_addon(wlr_addon* a) { return reinterpret_cast<AddonHook*>(a)->self; }

void tree_addon_destroy(wlr_addon* a) {
    // Its tree and node go with the scene node.
    SubsurfaceTree* t = from_scene_addon(a);
    if (t->parent)
        wlr_addon_finish(&t->surface_addon.addon);
    wlr_addon_finish(&t->scene_addon.addon);
    delete t;
}

void surface_addon_destroy(wlr_addon* a) {
    from_surface_addon(a)->tree->destroy();
}

SubsurfaceTree* child_of(SubsurfaceTree* parent, wlr_subsurface* sub) {
    wlr_addon* a = wlr_addon_find(&sub->surface->addons, parent, &kSurfaceAddon);
    assert(a);
    return from_surface_addon(a);
}

bool reconfigure_clip(SubsurfaceTree* t) {
    if (t->parent)
        t->clip = {t->parent->clip.x - t->tree->x, t->parent->clip.y - t->tree->y, t->parent->clip.width,
                   t->parent->clip.height};
    Buffer* b = t->node->buffer;
    if (wlr_box_empty(&t->clip)) {
        t->node->set_clip(nullptr);
        b->set_enabled(true);
        b->set_position(0, 0);
        return false;
    }
    wlr_box clip = t->clip;
    const wlr_box surface_box{0, 0, t->surface->current.width, t->surface->current.height};
    const bool meets = wlr_box_intersection(&clip, &clip, &surface_box);
    b->set_enabled(meets);
    if (meets) {
        b->set_position(clip.x, clip.y);
        t->node->set_clip(&clip);
    }
    return true;
}

void reconfigure(SubsurfaceTree* t) {
    const bool clipped = reconfigure_clip(t);
    wlr_surface* surface = t->surface;
    Node* prev = nullptr;
    wlr_subsurface* sub;
    wl_list_for_each(sub, &surface->current.subsurfaces_below, current.link) {
        SubsurfaceTree* c = child_of(t, sub);
        if (prev)
            c->tree->place_above(prev);
        prev = c->tree;
        c->tree->set_position(sub->current.x, sub->current.y);
        if (clipped)
            reconfigure_clip(c);
    }
    if (prev)
        t->node->buffer->place_above(prev);
    prev = t->node->buffer;
    wl_list_for_each(sub, &surface->current.subsurfaces_above, current.link) {
        SubsurfaceTree* c = child_of(t, sub);
        c->tree->place_above(prev);
        prev = c->tree;
        c->tree->set_position(sub->current.x, sub->current.y);
        if (clipped)
            reconfigure_clip(c);
    }
}

SubsurfaceTree* surface_tree_create(Tree* parent, wlr_surface* surface);

bool create_child(SubsurfaceTree* parent, wlr_subsurface* sub) {
    SubsurfaceTree* c = surface_tree_create(parent->tree, sub->surface);
    if (!c)
        return false;
    c->parent = parent;
    c->surface_addon.self = c;
    wlr_addon_init(&c->surface_addon.addon, &sub->surface->addons, parent, &kSurfaceAddon);
    c->subsurface_destroy.connect(&sub->events.destroy, [c](void*) { c->tree->destroy(); });
    return true;
}

SubsurfaceTree* surface_tree_create(Tree* parent, wlr_surface* surface) {
    auto* t = new SubsurfaceTree();
    t->tree = Tree::create(parent);
    t->node = SurfaceNode::create(t->tree, surface);
    t->surface = surface;
    t->scene_addon.self = t;
    wlr_addon_init(&t->scene_addon.addon, &t->tree->addons, nullptr, &kTreeAddon);

    wlr_subsurface* sub;
    wl_list_for_each(sub, &surface->current.subsurfaces_below, current.link)
        create_child(t, sub);
    wl_list_for_each(sub, &surface->current.subsurfaces_above, current.link)
        create_child(t, sub);
    reconfigure(t);

    t->surface_destroy.connect(&surface->events.destroy, [t](void*) { t->tree->destroy(); });
    // TODO(upstream too): only on a change of order or position.
    t->surface_commit.connect(&surface->events.commit, [t](void*) { reconfigure(t); });
    t->surface_map.connect(&surface->events.map, [t](void*) { t->tree->set_enabled(true); });
    t->surface_unmap.connect(&surface->events.unmap, [t](void*) { t->tree->set_enabled(false); });
    t->new_subsurface.connect(&surface->events.new_subsurface, [t](wlr_subsurface* s) {
        if (!create_child(t, s))
            wl_resource_post_no_memory(s->resource);
    });
    t->tree->set_enabled(surface->mapped);
    return t;
}

bool set_clip(Node* node, const wlr_box* clip) {
    if (node->type != Type::Tree)
        return false;
    bool found = false;
    if (wlr_addon* a = wlr_addon_find(&node->addons, nullptr, &kTreeAddon)) {
        SubsurfaceTree* t = from_scene_addon(a);
        if (!t->parent) {
            const wlr_box next = clip ? *clip : wlr_box{};
            if (wlr_box_equal(&t->clip, &next))
                return true;
            t->clip = next;
        }
        found = true;
        reconfigure_clip(t);
    }
    Node* child;
    wl_list_for_each(child, &static_cast<Tree*>(node)->children, link)
        found = set_clip(child, clip) || found;
    return found;
}

} // namespace

Tree* subsurface_tree_create(Tree* parent, wlr_surface* surface) {
    return surface_tree_create(parent, surface)->tree;
}

void subsurface_tree_set_clip(Node* node, const wlr_box* clip) {
    [[maybe_unused]] const bool found = set_clip(node, clip);
    assert(found);
}

// ---- xdg surfaces -------------------------------------------------------------

namespace {

struct XdgSurfaceNode {
    Tree* tree;
    Tree* surface_tree;
    wlr_xdg_surface* xdg;
    Listener<> tree_destroy, xdg_destroy, commit;

    void update_position() {
        surface_tree->set_position(-xdg->geometry.x, -xdg->geometry.y);
        if (xdg->role == WLR_XDG_SURFACE_ROLE_POPUP && xdg->popup)
            tree->set_position(xdg->popup->current.geometry.x, xdg->popup->current.geometry.y);
    }
};

} // namespace

Tree* xdg_surface_create(Tree* parent, wlr_xdg_surface* xdg) {
    auto* x = new XdgSurfaceNode();
    x->xdg = xdg;
    x->tree = Tree::create(parent);
    x->surface_tree = subsurface_tree_create(x->tree, xdg->surface);
    x->tree_destroy.connect(&x->tree->events.destroy, [x](void*) { delete x; });
    x->xdg_destroy.connect(&xdg->events.destroy, [x](void*) { x->tree->destroy(); });
    x->commit.connect(&xdg->surface->events.commit, [x](void*) { x->update_position(); });
    x->update_position();
    return x->tree;
}

// ---- layer surfaces -----------------------------------------------------------

namespace {

struct LayerNodeImpl {
    LayerSurfaceNode pub;
    Listener<> tree_destroy, layer_destroy;
};

void exclusive_zone(const wlr_layer_surface_v1_state* s, wlr_edges edge, wlr_box* usable) {
    switch (edge) {
    case WLR_EDGE_NONE:
        return;
    case WLR_EDGE_TOP:
        usable->y += s->exclusive_zone + s->margin.top;
        usable->height -= s->exclusive_zone + s->margin.top;
        break;
    case WLR_EDGE_BOTTOM:
        usable->height -= s->exclusive_zone + s->margin.bottom;
        break;
    case WLR_EDGE_LEFT:
        usable->x += s->exclusive_zone + s->margin.left;
        usable->width -= s->exclusive_zone + s->margin.left;
        break;
    case WLR_EDGE_RIGHT:
        usable->width -= s->exclusive_zone + s->margin.right;
        break;
    }
    usable->width = std::max(usable->width, 0);
    usable->height = std::max(usable->height, 0);
}

} // namespace

LayerSurfaceNode* layer_surface_v1_create(Tree* parent, wlr_layer_surface_v1* layer) {
    auto* l = new LayerNodeImpl();
    l->pub.layer_surface = layer;
    l->pub.tree = Tree::create(parent);
    subsurface_tree_create(l->pub.tree, layer->surface);
    l->tree_destroy.connect(&l->pub.tree->events.destroy, [l](void*) { delete l; });
    l->layer_destroy.connect(&layer->events.destroy, [l](void*) { l->pub.tree->destroy(); });
    return &l->pub;
}

void layer_surface_v1_configure(LayerSurfaceNode* node, const wlr_box* full_area, wlr_box* usable_area) {
    wlr_layer_surface_v1* layer = node->layer_surface;
    const wlr_layer_surface_v1_state* s = &layer->current;
    // Exclusive zone -1: the whole output, else what's left of it.
    const wlr_box bounds = s->exclusive_zone == -1 ? *full_area : *usable_area;
    wlr_box box{0, 0, int(s->desired_width), int(s->desired_height)};
    const uint32_t a = s->anchor;
    constexpr uint32_t L = ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT, R = ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT;
    constexpr uint32_t T = ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP, B = ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM;
    if (box.width == 0) {
        box.x = bounds.x + int(s->margin.left);
        box.width = bounds.width - int(s->margin.left + s->margin.right);
    } else if ((a & L) && (a & R)) {
        box.x = bounds.x + bounds.width / 2 - box.width / 2;
    } else if (a & L) {
        box.x = bounds.x + int(s->margin.left);
    } else if (a & R) {
        box.x = bounds.x + bounds.width - box.width - int(s->margin.right);
    } else {
        box.x = bounds.x + bounds.width / 2 - box.width / 2;
    }
    if (box.height == 0) {
        box.y = bounds.y + int(s->margin.top);
        box.height = bounds.height - int(s->margin.top + s->margin.bottom);
    } else if ((a & T) && (a & B)) {
        box.y = bounds.y + bounds.height / 2 - box.height / 2;
    } else if (a & T) {
        box.y = bounds.y + int(s->margin.top);
    } else if (a & B) {
        box.y = bounds.y + bounds.height - box.height - int(s->margin.bottom);
    } else {
        box.y = bounds.y + bounds.height / 2 - box.height / 2;
    }
    node->tree->set_position(box.x, box.y);
    wlr_layer_surface_v1_configure(layer, uint32_t(box.width), uint32_t(box.height));
    if (layer->surface->mapped && s->exclusive_zone > 0)
        exclusive_zone(s, wlr_layer_surface_v1_get_exclusive_edge(layer), usable_area);
}

// ---- drag icons ---------------------------------------------------------------

namespace {

struct DragIconNode {
    Tree* tree;
    Tree* surface_tree;
    wlr_drag_icon* icon;
    Listener<> tree_destroy, commit, icon_destroy;
};

} // namespace

Tree* drag_icon_create(Tree* parent, wlr_drag_icon* icon) {
    auto* d = new DragIconNode();
    d->icon = icon;
    d->tree = Tree::create(parent);
    d->surface_tree = subsurface_tree_create(d->tree, icon->surface);
    d->tree_destroy.connect(&d->tree->events.destroy, [d](void*) { delete d; });
    d->commit.connect(&icon->surface->events.commit, [d](void*) {
        wlr_surface* s = d->icon->surface;
        d->surface_tree->set_position(d->surface_tree->x + s->current.dx, d->surface_tree->y + s->current.dy);
    });
    d->icon_destroy.connect(&icon->events.destroy, [d](void*) { d->tree->destroy(); });
    return d->tree;
}

} // namespace atrium::scene
