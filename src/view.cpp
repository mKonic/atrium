#include "view.hpp"

#include "geometry.hpp"
#include "output.hpp"
#include "overview.hpp"
#include "seat.hpp"
#include "server.hpp"
#include "space.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <vector>

namespace atrium {

View::View(Server& srv, Kind k) : server(srv), kind(k), id(srv.next_view_id++) {}

View::~View() {
    server.animator.cancel_owner(this, false);
    destroy_toplevel_handles();
}

void View::place_tree() {
    if (tree)
        wlr_scene_node_set_position(&tree->node, geom.x + anim_dx_, geom.y + anim_dy_);
}

void View::set_anim_offset(int dx, int dy) {
    anim_dx_ = dx;
    anim_dy_ = dy;
    place_tree();
}

void View::set_alpha(float a) {
    alpha_ = a;
    if (!tree || unmanaged())
        return;
    update_decorations();  // shadow, outline, title bar and content all follow alpha_
}

bool View::visible() const {
    return mapped && !minimized && (!space || space->shown());
}

wlr_box View::usable_area() const {
    if (output)
        return output->usable;
    if (server.focused_output)
        return server.focused_output->usable;
    return server.layout_box;
}

// --- map / unmap ---------------------------------------------------------

void View::handle_map() {
    tree = wlr_scene_tree_create(server.layer(unmanaged() ? Layer::Unmanaged : Layer::Views));
    content = create_content(tree);
    popups = wlr_scene_tree_create(tree);
    surface()->data = popups;  // parent tree for this window's popups
    tree->node.data = content->node.data = popups->node.data = this;
    mapped = true;

    if (unmanaged()) {
        // Menus and tooltips place themselves.
        place_tree();
        if (wants_focus())
            server.focus_view(this);
        return;
    }

    const Config& c = server.config;
    shadow = wlr_scene_shadow_create(tree, 0, 0, c.corner_radius, c.shadow_sigma,
                                     c.shadow_color.data());
    wlr_scene_node_lower_to_bottom(&shadow->node);
    outline = wlr_scene_rect_create(tree, 0, 0, premultiplied(c.outline_color).data());
    outline->accepts_input = false;
    wlr_scene_node_place_above(&outline->node, &shadow->node);
    blur = wlr_scene_blur_create(tree, 0, 0);
    wlr_scene_blur_set_should_only_blur_bottom_layer(blur, true);  // the cheap, shared background blur
    wlr_scene_node_place_above(&blur->node, &outline->node);

    if (wants_ssd()) {
        titlebar = std::make_unique<Titlebar>(*this, tree);
        geom.height += top();
    }
    layout_frame();

    const RuleResult wish = server.assign_space(this);
    if (space)
        wlr_scene_node_reparent(&tree->node, space->tree);

    server.views.insert(server.views.begin(), this);
    create_toplevel_handles();
    place();
    update_decorations();
    server.notify_window(*this, "opened");
    if (wish.fullscreen.value_or(false))
        set_fullscreen(true);
    else if (wish.maximized.value_or(false))
        set_maximized(true);
    // A window a rule sent to a space you aren't looking at opens quietly.
    if (!space || space->shown())
        server.focus_view(this);
    else
        server.spaces_changed();

    // Fade in, rising into place.
    server.animator.start(this, 220, Ease::OutQuint, [this](double t) {
        set_alpha(float(t));
        set_anim_offset(0, int(std::lround((1 - t) * 14)));
    });
    server.overview->view_mapped(this);
}

void View::handle_unmap() {
    if (server.overview)
        server.overview->view_unmapped(this);
    server.animator.cancel_owner(this, false);
    if (!unmanaged() && visible())
        animate_close();
    alpha_ = 1.0f;
    anim_dx_ = anim_dy_ = 0;
    server.seat->view_unmapped(this);
    const bool was_focused = server.focused_view == this;
    if (was_focused)
        server.focused_view = nullptr;

    if (!unmanaged()) {
        server.notify_window(*this, "closed");
        std::erase(server.views, this);
        destroy_toplevel_handles();
    }

    Output* old_output = output;
    const bool was_fullscreen = fullscreen;

    titlebar.reset();
    Space* old_space = space;
    space = nullptr;
    wlr_scene_node_destroy(&tree->node);
    tree = content = popups = nullptr;
    shadow = nullptr;
    outline = nullptr;
    blur = nullptr;
    surface()->data = nullptr;
    mapped = false;
    // A window that comes back starts fresh; only its last geometry survives.
    minimized = maximized = fullscreen = activated = false;
    snapped = 0;
    resize_edges_ = 0;
    resize_settling_ = false;

    if (was_fullscreen && old_output)
        old_output->refit_views();
    server.prune_space(old_space);
    if (was_focused || (unmanaged() && wants_focus()))
        server.focus_top();
    server.seat->refresh_pointer();
}

// Centered on the parent for dialogs, centered on the output otherwise, and
// stepped down-right when that exact spot is already taken, like macOS.
void View::place() {
    View* p = parent();
    Output* target = nullptr;
    if (space && !space->secret)
        target = space->output;
    else if (space && space->secret)
        target = space->output ? space->output : server.focused_output;
    if (p && p->output && p->space == space)
        target = p->output;
    if (!target)
        target = server.focused_output;
    if (!target) {
        double cx = server.seat->cursor->x, cy = server.seat->cursor->y;
        target = server.output_at(cx, cy);
    }
    set_output(target);

    std::vector<wlr_box> others;
    for (View* v : server.views)
        if (v != this && v->mapped && !v->minimized && v->space == space)
            others.push_back(v->geom);
    const wlr_box* parent_box = (p && p->mapped) ? &p->geom : nullptr;
    const wlr_box g = geometry::place(geom.width, geom.height, usable_area(), parent_box, others,
                                      server.config.cascade_step);

    if (g.width != geom.width || g.height != geom.height)
        request_geometry(g);
    else
        move_to(g.x, g.y);
}

// --- closing ---------------------------------------------------------------------

namespace {

// What stays on screen for a moment after a window is gone: copies of its last
// buffers, shadow and outline, fading out on their own.
struct Ghost {
    wlr_scene_tree* tree = nullptr;
    int origin_x = 0, origin_y = 0;  // for_each_buffer counts the root's own position
    std::vector<wlr_scene_buffer*> buffers;
    std::vector<float> opacity;  // each buffer's own opacity at close
    wlr_scene_shadow* shadow = nullptr;
    wlr_scene_rect* outline = nullptr;
    Color shadow_color{}, outline_color{};
    int x = 0, y = 0;
};

void copy_into_ghost(wlr_scene_buffer* src, int sx, int sy, void* data) {
    auto* g = static_cast<Ghost*>(data);
    if (!src->buffer)
        return;
    wlr_scene_buffer* dst = wlr_scene_buffer_create(g->tree, src->buffer);
    wlr_scene_node_set_position(&dst->node, sx - g->origin_x, sy - g->origin_y);
    wlr_scene_buffer_set_source_box(dst, &src->src_box);
    wlr_scene_buffer_set_dest_size(dst, src->dst_width, src->dst_height);
    wlr_scene_buffer_set_transform(dst, src->transform);
    wlr_scene_buffer_set_corner_radii(dst, src->corners);
    g->buffers.push_back(dst);
    g->opacity.push_back(src->opacity);
}

} // namespace

// Called while the window's scene is still intact, just before it goes.
void View::animate_close() {
    if (!tree || !server.config.animations)
        return;
    auto* g = new Ghost;
    // Straight under the layer, not the space: the space may be pruned while
    // the ghost is still fading.
    g->tree = wlr_scene_tree_create(server.layer(fullscreen ? Layer::Fullscreen : Layer::Views));
    g->x = tree->node.x;
    g->y = tree->node.y;
    wlr_scene_node_set_position(&g->tree->node, g->x, g->y);

    const Config& c = server.config;
    if (shadow && shadow->node.enabled) {
        g->shadow_color = activated ? c.shadow_color : c.shadow_color_inactive;
        g->shadow = wlr_scene_shadow_create(g->tree, shadow->width, shadow->height, shadow->corner_radius,
                                            shadow->blur_sigma, g->shadow_color.data());
        wlr_scene_node_set_position(&g->shadow->node, shadow->node.x, shadow->node.y);
        wlr_scene_shadow_set_clipped_region(g->shadow, shadow->clipped_region);
    }
    if (outline && outline->node.enabled) {
        g->outline_color = activated ? c.outline_color : c.outline_color_inactive;
        g->outline = wlr_scene_rect_create(g->tree, outline->width, outline->height, premultiplied(g->outline_color).data());
        g->outline->accepts_input = false;
        wlr_scene_node_set_position(&g->outline->node, outline->node.x, outline->node.y);
        wlr_scene_rect_set_corner_radii(g->outline, outline->corners);
        wlr_scene_rect_set_clipped_region(g->outline, outline->clipped_region);
    }
    g->origin_x = tree->node.x;
    g->origin_y = tree->node.y;
    wlr_scene_node_for_each_buffer(&tree->node, copy_into_ghost, g);

    server.animator.start(g, 160, Ease::InCubic, [g](double t) {
        const float a = float(1 - t);
        for (size_t i = 0; i < g->buffers.size(); ++i)
            wlr_scene_buffer_set_opacity(g->buffers[i], g->opacity[i] * a);
        if (g->shadow) {
            Color sc = g->shadow_color;
            sc[3] *= a;
            wlr_scene_shadow_set_color(g->shadow, sc.data());
        }
        if (g->outline) {
            Color oc = g->outline_color;
            oc[3] *= a;
            wlr_scene_rect_set_color(g->outline, premultiplied(oc).data());
        }
        wlr_scene_node_set_position(&g->tree->node, g->x, g->y + int(std::lround(t * 10)));
    }, [g] {
        wlr_scene_node_destroy(&g->tree->node);
        delete g;
    });
}

// --- geometry ------------------------------------------------------------------

void View::move_to(int x, int y) {
    geom.x = x;
    geom.y = y;
    place_tree();
    notify_position();
    update_output_from_position();
}

void View::request_geometry(wlr_box box) {
    // Hints limit the client's content, not the frame around it.
    wlr_box min{}, max{};
    size_hints(min, max);
    const wlr_box inner = geometry::clamp_to_hints(content_box(box), min, max);
    box.width = inner.width;
    box.height = inner.height + top();

    // During an interactive resize the position follows the committed size
    // (see handle_size); moving now would make the window jump ahead of it.
    if (!anchored()) {
        geom.x = box.x;
        geom.y = box.y;
        place_tree();
        update_output_from_position();
    }
    configure(box);
}

void View::handle_size(int width, int height) {
    height += top();
    if (width == geom.width && height == geom.height)
        return;
    if (anchored() && (resize_edges_ & WLR_EDGE_LEFT))
        geom.x = anchor_right_ - width;
    if (anchored() && (resize_edges_ & WLR_EDGE_TOP))
        geom.y = anchor_bottom_ - height;
    geom.width = width;
    geom.height = height;
    place_tree();
    update_decorations();
}

void View::begin_resize(uint32_t edges) {
    resize_edges_ = edges;
    resize_settling_ = false;
    anchor_right_ = geom.x + geom.width;
    anchor_bottom_ = geom.y + geom.height;
}

void View::end_resize() {
    if (awaiting_configure()) {
        resize_settling_ = true;
        return;
    }
    settle_resize();
}

void View::settle_resize() {
    resize_edges_ = 0;
    resize_settling_ = false;
    update_output_from_position();
    server.notify_window(*this, "changed");
}

void View::update_output_from_position() {
    if (Output* o = server.output_at(geom.x + geom.width / 2.0, geom.y + geom.height / 2.0))
        set_output(o);
}

void View::set_output(Output* o) {
    if (!o || o == output)
        return;
    if (handle_ && output)
        wlr_foreign_toplevel_handle_v1_output_leave(handle_, output->wlr);
    output = o;
    if (handle_)
        wlr_foreign_toplevel_handle_v1_output_enter(handle_, output->wlr);
    if (wlr_surface* s = surface()) {
        wlr_fractional_scale_v1_notify_scale(s, o->wlr->scale);
        wlr_surface_set_preferred_buffer_scale(s, int32_t(std::ceil(o->wlr->scale)));
    }
}

int View::top() const {
    return (titlebar && !fullscreen) ? titlebar->height() : 0;
}

// Content and popups sit below the title bar.
void View::layout_frame() {
    if (!tree)
        return;
    wlr_scene_node_set_position(&content->node, 0, top());
    wlr_scene_node_set_position(&popups->node, 0, top());
    if (titlebar) {
        wlr_scene_node_set_enabled(&titlebar->node()->node, top() > 0);
        titlebar->update();
    }
}

void View::refresh_decoration_mode() {
    if (!mapped || unmanaged())
        return;
    const bool want = wants_ssd();
    if (want == bool(titlebar))
        return;
    // The content keeps its size; the frame grows or shrinks by the bar.
    const int old_top = top();
    if (want) {
        titlebar = std::make_unique<Titlebar>(*this, tree);
    } else {
        server.seat->titlebar_gone(titlebar.get());
        titlebar.reset();
    }
    geom.height += top() - old_top;
    layout_frame();
    update_decorations();
    server.seat->refresh_pointer();
}

// --- state -------------------------------------------------------------------

void View::raise() {
    if (tree)
        wlr_scene_node_raise_to_top(&tree->node);
}

void View::set_activated(bool a) {
    activated = a;
    send_activated(a);
    if (handle_)
        wlr_foreign_toplevel_handle_v1_set_activated(handle_, a);
    update_decorations();
}

void View::set_maximized(bool m, bool restore_geometry) {
    if (m == maximized || unmanaged())
        return;
    maximized = m;
    send_maximized(m);
    if (handle_)
        wlr_foreign_toplevel_handle_v1_set_maximized(handle_, m);
    server.notify_window(*this, "changed");
    if (fullscreen)
        return;  // takes effect when fullscreen ends
    if (m) {
        if (!snapped)
            restore = geom;  // a snapped window already remembers where it was
        snapped = 0;
        request_geometry(usable_area());
    } else if (restore_geometry) {
        request_geometry(restore);
    }
}

void View::snap(uint32_t zone) {
    if (!zone || unmanaged() || fullscreen || !mapped)
        return;
    if (zone == WLR_EDGE_TOP) {
        set_maximized(true);
        return;
    }
    if (maximized)
        set_maximized(false, false);
    else if (!snapped)
        restore = geom;
    snapped = zone;
    request_geometry(geometry::snap_box(usable_area(), zone, server.config.snap_gap));
    server.notify_window(*this, "changed");
}

void View::unsnap(bool restore_geometry) {
    if (!snapped)
        return;
    snapped = 0;
    if (restore_geometry)
        request_geometry(restore);
    server.notify_window(*this, "changed");
}

void View::set_fullscreen(bool f) {
    if (f == fullscreen || unmanaged() || !tree)
        return;
    if (f && !maximized)
        restore = geom;  // with its title bar, before it hides
    const int old_top = top();
    fullscreen = f;
    geom.height += top() - old_top;  // the bar hides while fullscreen
    layout_frame();
    send_fullscreen(f);
    if (handle_)
        wlr_foreign_toplevel_handle_v1_set_fullscreen(handle_, f);

    if (f) {
        wlr_scene_node_reparent(&tree->node, space ? space->fullscreen_tree : server.layer(Layer::Fullscreen));
        if (output)
            request_geometry(output->box);
    } else {
        wlr_scene_node_reparent(&tree->node, space ? space->tree : server.layer(Layer::Views));
        request_geometry(maximized ? usable_area() : restore);
    }
    update_decorations();
    if (output)
        output->refit_views();
    server.notify_window(*this, "changed");
}

void View::set_minimized(bool m) {
    if (m == minimized || unmanaged() || !tree)
        return;
    minimized = m;
    // Sink and fade toward the bottom of the screen, or come back from it. The
    // tree stays enabled while it animates out.
    server.animator.cancel_owner(this, false);
    wlr_scene_node_set_enabled(&tree->node, true);
    if (m) {
        server.animator.start(this, 200, Ease::InCubic, [this](double t) {
            set_alpha(float(1 - t));
            set_anim_offset(0, int(std::lround(t * 40)));
        }, [this] {
            if (minimized && tree)
                wlr_scene_node_set_enabled(&tree->node, false);
            set_alpha(1.0f);
            set_anim_offset(0, 0);
        });
    } else {
        server.animator.start(this, 240, Ease::OutQuint, [this](double t) {
            set_alpha(float(t));
            set_anim_offset(0, int(std::lround((1 - t) * 40)));
        });
    }
    send_suspended(m);
    if (handle_)
        wlr_foreign_toplevel_handle_v1_set_minimized(handle_, m);
    server.notify_window(*this, "changed");
    if (fullscreen && output)
        output->refit_views();

    if (m) {
        if (server.focused_view == this) {
            set_activated(false);
            server.focused_view = nullptr;
            server.focus_top();
        }
    } else {
        server.focus_view(this);
    }
    server.seat->refresh_pointer();
}

// --- decorations -------------------------------------------------------------

namespace {

struct RoundCtx {
    int ox, oy;         // the content node's own position, which for_each_buffer adds
    int width, height;  // the content, in content coordinates
    int radius;
    bool round_top;     // false under atrium's title bar, which carries the top corners
    float alpha;
};

// Round exactly the buffer corners that sit on a corner of the window. Clients
// build windows out of several surfaces (foot draws its title bar as a
// subsurface, GTK pads the main surface with its own shadow), so no single
// buffer is "the window": what matters is which visible pixels form its corners.
void round_window_corners(wlr_scene_buffer* buffer, int sx, int sy, void* data) {
    auto* ctx = static_cast<RoundCtx*>(data);
    sx -= ctx->ox;
    sy -= ctx->oy;
    if (!wlr_scene_surface_try_from_buffer(buffer))
        return;
    int w = buffer->dst_width, h = buffer->dst_height;
    if ((w <= 0 || h <= 0) && buffer->buffer) {
        w = buffer->buffer->width;
        h = buffer->buffer->height;
    }
    // Visible part after the clip to window geometry.
    const int x0 = std::max(sx, 0), y0 = std::max(sy, 0);
    const int x1 = std::min(sx + w, ctx->width), y1 = std::min(sy + h, ctx->height);
    const int r = ctx->radius;
    const bool left = x0 == 0, right = x1 == ctx->width, top = y0 == 0 && ctx->round_top,
               bottom = y1 == ctx->height;
    wlr_scene_buffer_set_corner_radii(buffer, corner_radii_new(
        top && left ? r : 0, top && right ? r : 0, bottom && right ? r : 0, bottom && left ? r : 0));
    wlr_scene_buffer_set_opacity(buffer, ctx->alpha);
}

} // namespace

// Rounded corners and a soft shadow on every managed window, stronger on the
// focused one. Nothing while fullscreen.
void View::update_decorations() {
    if (!tree || !shadow || !outline || !blur || unmanaged())
        return;
    const Config& c = server.config;
    const int radius = fullscreen ? 0 : c.corner_radius;
    const bool show_shadow = c.shadows && !fullscreen;
    wlr_scene_node_set_enabled(&shadow->node, show_shadow);
    if (show_shadow) {
        const float sigma = activated ? c.shadow_sigma : c.shadow_sigma_inactive;
        const int margin = int(std::ceil(sigma));
        wlr_scene_shadow_set_blur_sigma(shadow, sigma);
        Color sc = activated ? c.shadow_color : c.shadow_color_inactive;
        sc[3] *= alpha_;
        wlr_scene_shadow_set_color(shadow, sc.data());
        wlr_scene_shadow_set_corner_radius(shadow, radius);
        wlr_scene_shadow_set_size(shadow, geom.width + 2 * margin, geom.height + 2 * margin);
        wlr_scene_node_set_position(&shadow->node, -margin, -margin);
        // Cut the window's own area out, so a translucent window does not
        // show its shadow through itself.
        wlr_scene_shadow_set_clipped_region(shadow, clipped_region{
            .area = {margin, margin, geom.width, geom.height},
            .corners = corner_radii_all(radius),
        });
    }

    // A faint light hairline keeps dark windows distinct on a dark desktop,
    // where a shadow alone disappears.
    wlr_scene_node_set_enabled(&outline->node, !fullscreen && c.outline_color[3] > 0);
    if (!fullscreen) {
        Color oc = activated ? c.outline_color : c.outline_color_inactive;
        oc[3] *= alpha_;
        wlr_scene_rect_set_color(outline, premultiplied(oc).data());
        wlr_scene_rect_set_size(outline, geom.width + 2, geom.height + 2);
        wlr_scene_node_set_position(&outline->node, -1, -1);
        wlr_scene_rect_set_corner_radius(outline, radius > 0 ? radius + 1 : 0);
        wlr_scene_rect_set_clipped_region(outline, clipped_region{
            .area = {1, 1, geom.width, geom.height},
            .corners = corner_radii_all(radius),
        });
    }

    const bool show_blur = c.blur && !fullscreen;
    wlr_scene_node_set_enabled(&blur->node, show_blur);
    if (show_blur) {
        wlr_scene_blur_set_size(blur, geom.width, geom.height);
        wlr_scene_blur_set_corner_radius(blur, radius);
        wlr_scene_blur_set_alpha(blur, alpha_);
    }

    if (titlebar) {
        titlebar->update();
        wlr_scene_buffer_set_opacity(titlebar->node(), alpha_);
    }
    update_corners();
}

void View::update_corners() {
    if (!content || unmanaged())
        return;
    RoundCtx ctx{content->node.x, content->node.y, geom.width, geom.height - top(), fullscreen ? 0 : server.config.corner_radius,
                 top() == 0, alpha_};
    wlr_scene_node_for_each_buffer(&content->node, round_window_corners, &ctx);
    if (server.overview)
        server.overview->view_changed(this);
}

// --- foreign toplevel handles (docks, task switchers, screen sharing) ---------

void View::create_toplevel_handles() {
    wlr_ext_foreign_toplevel_handle_v1_state state{};
    state.title = title();
    state.app_id = app_id();
    ext_handle_ = wlr_ext_foreign_toplevel_handle_v1_create(server.ext_toplevel_list, &state);
    ext_handle_->data = this;

    handle_ = wlr_foreign_toplevel_handle_v1_create(server.toplevel_manager);
    handle_->data = this;
    wlr_foreign_toplevel_handle_v1_set_title(handle_, title());
    wlr_foreign_toplevel_handle_v1_set_app_id(handle_, app_id());
    if (output)
        wlr_foreign_toplevel_handle_v1_output_enter(handle_, output->wlr);
    if (View* p = parent(); p && p->handle_)
        wlr_foreign_toplevel_handle_v1_set_parent(handle_, p->handle_);

    handle_activate_.connect(&handle_->events.request_activate, [this](auto*) {
        if (minimized)
            set_minimized(false);
        server.focus_view(this);
    });
    handle_maximize_.connect(&handle_->events.request_maximize,
        [this](wlr_foreign_toplevel_handle_v1_maximized_event* e) { set_maximized(e->maximized); });
    handle_minimize_.connect(&handle_->events.request_minimize,
        [this](wlr_foreign_toplevel_handle_v1_minimized_event* e) { set_minimized(e->minimized); });
    handle_fullscreen_.connect(&handle_->events.request_fullscreen,
        [this](wlr_foreign_toplevel_handle_v1_fullscreen_event* e) { set_fullscreen(e->fullscreen); });
    handle_close_.connect(&handle_->events.request_close, [this](void*) { close(); });

    // A private scene holding just this window, for per-window screen capture.
    capture_scene_ = wlr_scene_create();
    create_content(&capture_scene_->tree);
}

void View::destroy_toplevel_handles() {
    handle_activate_.disconnect();
    handle_maximize_.disconnect();
    handle_minimize_.disconnect();
    handle_fullscreen_.disconnect();
    handle_close_.disconnect();
    if (handle_) {
        wlr_foreign_toplevel_handle_v1_destroy(handle_);
        handle_ = nullptr;
    }
    if (ext_handle_) {
        wlr_ext_foreign_toplevel_handle_v1_destroy(ext_handle_);
        ext_handle_ = nullptr;
    }
    if (capture_scene_) {
        wlr_scene_node_destroy(&capture_scene_->tree.node);
        capture_scene_ = nullptr;
        capture_source_ = nullptr;  // owned by the scene node
    }
}

void View::update_title() {
    if (ext_handle_) {
        wlr_ext_foreign_toplevel_handle_v1_state state{};
        state.title = title();
        state.app_id = app_id();
        wlr_ext_foreign_toplevel_handle_v1_update_state(ext_handle_, &state);
    }
    if (handle_) {
        wlr_foreign_toplevel_handle_v1_set_title(handle_, title());
        wlr_foreign_toplevel_handle_v1_set_app_id(handle_, app_id());
    }
    if (titlebar)
        titlebar->update();
    if (mapped)
        server.notify_window(*this, "changed");
}

} // namespace atrium
