#include "view.hpp"

#include "output.hpp"
#include "seat.hpp"
#include "server.hpp"

#include <algorithm>
#include <cmath>

namespace atrium {

View::View(Server& srv, Kind k) : server(srv), kind(k) {}

View::~View() {
    destroy_toplevel_handles();
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
    surface()->data = tree;  // parent tree for this window's popups
    content = create_content(tree);
    tree->node.data = content->node.data = this;
    mapped = true;

    if (unmanaged()) {
        // Menus and tooltips place themselves.
        wlr_scene_node_set_position(&tree->node, geom.x, geom.y);
        if (wants_focus())
            server.focus_view(this);
        return;
    }

    const Config& c = server.config;
    shadow = wlr_scene_shadow_create(tree, 0, 0, c.corner_radius, c.shadow_sigma,
                                     c.shadow_color.data());
    wlr_scene_node_lower_to_bottom(&shadow->node);

    server.views.insert(server.views.begin(), this);
    create_toplevel_handles();
    place();
    update_decorations();
    server.focus_view(this);
}

void View::handle_unmap() {
    server.seat->view_unmapped(this);
    const bool was_focused = server.focused_view == this;
    if (was_focused)
        server.focused_view = nullptr;

    if (!unmanaged()) {
        std::erase(server.views, this);
        destroy_toplevel_handles();
    }

    Output* old_output = output;
    const bool was_fullscreen = fullscreen;

    wlr_scene_node_destroy(&tree->node);
    tree = content = nullptr;
    shadow = nullptr;
    surface()->data = nullptr;
    mapped = false;
    // A window that comes back starts fresh; only its last geometry survives.
    minimized = maximized = fullscreen = activated = false;
    resize_edges_ = 0;
    resize_settling_ = false;

    if (was_fullscreen && old_output)
        old_output->refit_views();
    if (was_focused || (unmanaged() && wants_focus()))
        server.focus_top();
    server.seat->refresh_pointer();
}

// Centered on the parent for dialogs, centered on the output otherwise, and
// stepped down-right when that exact spot is already taken, like macOS.
void View::place() {
    View* p = parent();
    Output* target = (p && p->output) ? p->output : server.focused_output;
    if (!target) {
        double cx = server.seat->cursor->x, cy = server.seat->cursor->y;
        target = server.output_at(cx, cy);
    }
    set_output(target);
    const wlr_box area = usable_area();

    wlr_box g = geom;
    g.width = std::min(g.width, area.width);
    g.height = std::min(g.height, area.height);

    if (p && p->mapped) {
        g.x = p->geom.x + (p->geom.width - g.width) / 2;
        g.y = p->geom.y + (p->geom.height - g.height) / 2;
    } else {
        g.x = area.x + (area.width - g.width) / 2;
        g.y = area.y + (area.height - g.height) / 2;
        const int step = server.config.cascade_step;
        for (int tries = 0; tries < 32; ++tries) {
            bool taken = std::ranges::any_of(server.views, [&](View* v) {
                return v != this && v->visible() && v->geom.x == g.x && v->geom.y == g.y;
            });
            if (!taken)
                break;
            g.x += step;
            g.y += step;
            if (g.x + g.width > area.x + area.width || g.y + g.height > area.y + area.height) {
                g.x = area.x;
                g.y = area.y;
            }
        }
    }

    g.x = std::clamp(g.x, area.x, std::max(area.x, area.x + area.width - g.width));
    g.y = std::clamp(g.y, area.y, std::max(area.y, area.y + area.height - g.height));

    if (g.width != geom.width || g.height != geom.height)
        request_geometry(g);
    else
        move_to(g.x, g.y);
}

// --- geometry ------------------------------------------------------------------

void View::move_to(int x, int y) {
    geom.x = x;
    geom.y = y;
    if (tree)
        wlr_scene_node_set_position(&tree->node, x, y);
    notify_position();
    update_output_from_position();
}

void View::request_geometry(wlr_box box) {
    wlr_box min{}, max{};
    size_hints(min, max);
    box.width = std::max({box.width, min.width, 1});
    box.height = std::max({box.height, min.height, 1});
    // Some clients advertise INT_MAX as their maximum; anything positive is a limit.
    if (max.width > 0)
        box.width = std::min(box.width, max.width);
    if (max.height > 0)
        box.height = std::min(box.height, max.height);

    // During an interactive resize the position follows the committed size
    // (see handle_size); moving now would make the window jump ahead of it.
    if (!anchored()) {
        geom.x = box.x;
        geom.y = box.y;
        if (tree)
            wlr_scene_node_set_position(&tree->node, geom.x, geom.y);
        update_output_from_position();
    }
    configure(box);
}

void View::handle_size(int width, int height) {
    if (width == geom.width && height == geom.height)
        return;
    if (anchored() && (resize_edges_ & WLR_EDGE_LEFT))
        geom.x = anchor_right_ - width;
    if (anchored() && (resize_edges_ & WLR_EDGE_TOP))
        geom.y = anchor_bottom_ - height;
    geom.width = width;
    geom.height = height;
    if (tree)
        wlr_scene_node_set_position(&tree->node, geom.x, geom.y);
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
    if (fullscreen)
        return;  // takes effect when fullscreen ends
    if (m) {
        restore = geom;
        request_geometry(usable_area());
    } else if (restore_geometry) {
        request_geometry(restore);
    }
}

void View::set_fullscreen(bool f) {
    if (f == fullscreen || unmanaged() || !tree)
        return;
    fullscreen = f;
    send_fullscreen(f);
    if (handle_)
        wlr_foreign_toplevel_handle_v1_set_fullscreen(handle_, f);

    if (f) {
        if (!maximized)
            restore = geom;
        wlr_scene_node_reparent(&tree->node, server.layer(Layer::Fullscreen));
        if (output)
            request_geometry(output->box);
    } else {
        wlr_scene_node_reparent(&tree->node, server.layer(Layer::Views));
        request_geometry(maximized ? usable_area() : restore);
    }
    update_decorations();
    if (output)
        output->refit_views();
}

void View::set_minimized(bool m) {
    if (m == minimized || unmanaged() || !tree)
        return;
    minimized = m;
    wlr_scene_node_set_enabled(&tree->node, !m);
    send_suspended(m);
    if (handle_)
        wlr_foreign_toplevel_handle_v1_set_minimized(handle_, m);
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
    int width, height;  // the window, in content coordinates
    int radius;
};

// Round exactly the buffer corners that sit on a corner of the window. Clients
// build windows out of several surfaces (foot draws its title bar as a
// subsurface, GTK pads the main surface with its own shadow), so no single
// buffer is "the window": what matters is which visible pixels form its corners.
void round_window_corners(wlr_scene_buffer* buffer, int sx, int sy, void* data) {
    auto* ctx = static_cast<RoundCtx*>(data);
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
    const bool left = x0 == 0, right = x1 == ctx->width, top = y0 == 0, bottom = y1 == ctx->height;
    wlr_scene_buffer_set_corner_radii(buffer, corner_radii_new(
        top && left ? r : 0, top && right ? r : 0, bottom && right ? r : 0, bottom && left ? r : 0));
}

} // namespace

// Rounded corners and a soft shadow on every managed window, stronger on the
// focused one. Nothing while fullscreen.
void View::update_decorations() {
    if (!tree || !shadow || unmanaged())
        return;
    const Config& c = server.config;
    const int radius = fullscreen ? 0 : c.corner_radius;
    const bool show_shadow = c.shadows && !fullscreen;
    wlr_scene_node_set_enabled(&shadow->node, show_shadow);
    if (show_shadow) {
        const float sigma = activated ? c.shadow_sigma : c.shadow_sigma_inactive;
        const int margin = int(std::ceil(sigma));
        wlr_scene_shadow_set_blur_sigma(shadow, sigma);
        wlr_scene_shadow_set_color(shadow, (activated ? c.shadow_color : c.shadow_color_inactive).data());
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

    update_corners();
}

void View::update_corners() {
    if (!content || unmanaged())
        return;
    RoundCtx ctx{geom.width, geom.height, fullscreen ? 0 : server.config.corner_radius};
    wlr_scene_node_for_each_buffer(&content->node, round_window_corners, &ctx);
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
}

} // namespace atrium
