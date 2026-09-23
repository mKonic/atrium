#ifdef ATRIUM_XWAYLAND
#include "xwayland_view.hpp"

#include "seat.hpp"
#include "server.hpp"

namespace atrium {

XwaylandView::XwaylandView(Server& srv, wlr_xwayland_surface* xs) : View(srv, Kind::X11), xsurface(xs) {
    xsurface->data = this;

    // The wlr_surface only exists between associate and dissociate.
    associate_.connect(&xsurface->events.associate, [this](void*) {
        map_.connect(&surface()->events.map, [this](void*) { map(); });
        unmap_.connect(&surface()->events.unmap, [this](void*) { unmap(); });
        commit_.connect(&surface()->events.commit, [this](void*) { commit(); });
    });
    dissociate_.connect(&xsurface->events.dissociate, [this](void*) {
        map_.disconnect();
        unmap_.disconnect();
        commit_.disconnect();
    });
    destroy_.connect(&xsurface->events.destroy, [this](void*) { delete this; });

    request_activate_.connect(&xsurface->events.request_activate, [this](void*) {
        if (!unmanaged())
            wlr_xwayland_surface_activate(xsurface, true);
    });
    request_configure_.connect(&xsurface->events.request_configure,
        [this](wlr_xwayland_surface_configure_event* e) { request_configure(e); });
    request_fullscreen_.connect(&xsurface->events.request_fullscreen, [this](void*) {
        if (mapped)
            set_fullscreen(xsurface->fullscreen);
    });
    request_maximize_.connect(&xsurface->events.request_maximize, [this](void*) {
        if (mapped)
            set_maximized(xsurface->maximized_horz || xsurface->maximized_vert);
    });
    request_minimize_.connect(&xsurface->events.request_minimize, [this](wlr_xwayland_minimize_event* e) {
        if (mapped)
            set_minimized(e->minimize);
    });
    request_close_.connect(&xsurface->events.request_close, [this](void*) { close(); });
    request_move_.connect(&xsurface->events.request_move, [this](void*) {
        if (mapped)
            server.seat->begin_move(this);
    });
    request_resize_.connect(&xsurface->events.request_resize, [this](wlr_xwayland_resize_event* e) {
        if (mapped)
            server.seat->begin_resize(this, e->edges);
    });
    set_geometry_.connect(&xsurface->events.set_geometry, [this](void*) { set_geometry(); });
    set_hints_.connect(&xsurface->events.set_hints, [this](void*) {
        if (this != server.focused_view && xsurface->hints)
            urgent = xcb_icccm_wm_hints_get_urgency(xsurface->hints);
    });
    set_title_.connect(&xsurface->events.set_title, [this](void*) { update_title(); });
    set_class_.connect(&xsurface->events.set_class, [this](void*) { update_title(); });
}

XwaylandView::~XwaylandView() {
    xsurface->data = nullptr;
}

void XwaylandView::map() {
    geom = {xsurface->x, xsurface->y, xsurface->width, xsurface->height};
    handle_map();
    if (unmanaged())
        return;
    if (xsurface->fullscreen)
        set_fullscreen(true);
    else if (xsurface->maximized_horz && xsurface->maximized_vert)
        set_maximized(true);
}

void XwaylandView::unmap() {
    handle_unmap();
}

void XwaylandView::commit() {
    if (mapped && !unmanaged()) {
        handle_size(surface()->current.width, surface()->current.height);
        update_corners();
    }
}

void XwaylandView::request_configure(wlr_xwayland_surface_configure_event* e) {
    if (!mapped || unmanaged()) {
        wlr_xwayland_surface_configure(xsurface, e->x, e->y, e->width, e->height);
        if (tree && unmanaged())
            wlr_scene_node_set_position(&tree->node, e->x, e->y);
        return;
    }
    // A floating X11 window may move and resize itself, except while the
    // compositor owns its geometry.
    if (fullscreen || maximized) {
        configure(geom);
        return;
    }
    request_geometry({e->x, e->y, e->width, e->height});
}

void XwaylandView::set_geometry() {
    if (!unmanaged() || !mapped)
        return;
    geom = {xsurface->x, xsurface->y, xsurface->width, xsurface->height};
    wlr_scene_node_set_position(&tree->node, geom.x, geom.y);
}

void XwaylandView::configure(const wlr_box& box) {
    wlr_xwayland_surface_configure(xsurface, int16_t(box.x), int16_t(box.y),
                                   uint16_t(box.width), uint16_t(box.height));
}

wlr_scene_tree* XwaylandView::create_content(wlr_scene_tree* parent) {
    return wlr_scene_subsurface_tree_create(parent, surface());
}

void XwaylandView::send_activated(bool a) {
    wlr_xwayland_surface_activate(xsurface, a);
    if (a)
        wlr_xwayland_surface_restack(xsurface, nullptr, XCB_STACK_MODE_ABOVE);
}

void XwaylandView::send_maximized(bool m) {
    wlr_xwayland_surface_set_maximized(xsurface, m, m);
}

void XwaylandView::send_fullscreen(bool f) {
    wlr_xwayland_surface_set_fullscreen(xsurface, f);
}

const char* XwaylandView::app_id() const {
    return xsurface->class_ ? xsurface->class_ : "";
}

const char* XwaylandView::title() const {
    return xsurface->title ? xsurface->title : "";
}

View* XwaylandView::parent() const {
    return xsurface->parent ? static_cast<View*>(xsurface->parent->data) : nullptr;
}

void XwaylandView::size_hints(wlr_box& min, wlr_box& max) const {
    min = max = {};
    if (auto* h = xsurface->size_hints) {
        min = {0, 0, h->min_width, h->min_height};
        max = {0, 0, h->max_width, h->max_height};
    }
}

bool XwaylandView::is_dialog() const {
    if (xsurface->modal || xsurface->parent)
        return true;
    for (auto type : {WLR_XWAYLAND_NET_WM_WINDOW_TYPE_DIALOG, WLR_XWAYLAND_NET_WM_WINDOW_TYPE_SPLASH,
                      WLR_XWAYLAND_NET_WM_WINDOW_TYPE_TOOLBAR, WLR_XWAYLAND_NET_WM_WINDOW_TYPE_UTILITY})
        if (wlr_xwayland_surface_has_window_type(xsurface, type))
            return true;
    wlr_box min, max;
    size_hints(min, max);
    return min.width > 0 && min.height > 0 && (min.width == max.width || min.height == max.height);
}

bool XwaylandView::wants_focus() const {
    return unmanaged() && wlr_xwayland_surface_override_redirect_wants_focus(xsurface) &&
           wlr_xwayland_surface_icccm_input_model(xsurface) != WLR_ICCCM_INPUT_MODEL_NONE;
}

void XwaylandView::close() {
    wlr_xwayland_surface_close(xsurface);
}

} // namespace atrium
#endif
