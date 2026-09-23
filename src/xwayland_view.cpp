#ifdef ATRIUM_XWAYLAND
#include "xwayland_view.hpp"

#include "seat.hpp"
#include "server.hpp"

namespace atrium {

XwaylandView::XwaylandView(Server& srv, wlr_xwayland_surface* xs) : View(srv, Kind::X11), xsurface(xs) {
    xsurface->data = this;

    // The wlr_surface only exists between associate and dissociate.
    wlr_log(WLR_DEBUG, "x11: window 0x%x created", xsurface->window_id);
    associate_.connect(&xsurface->events.associate, [this](void*) {
        wlr_log(WLR_DEBUG, "x11: window 0x%x has its surface (mapped: %d)", xsurface->window_id,
                surface()->mapped);
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
    set_decorations_.connect(&xsurface->events.set_decorations, [this](void*) { refresh_decoration_mode(); });
    auto states = [this](void*) {
        if (mapped && !unmanaged())
            apply_states();
    };
    request_above_.connect(&xsurface->events.request_above, states);
    request_below_.connect(&xsurface->events.request_below, states);
    request_sticky_.connect(&xsurface->events.request_sticky, states);
    request_skip_taskbar_.connect(&xsurface->events.request_skip_taskbar, states);
    request_attention_.connect(&xsurface->events.request_demands_attention, states);
}

XwaylandView::~XwaylandView() {
    xsurface->data = nullptr;
}

void XwaylandView::map() {
    wlr_log(WLR_DEBUG, "x11: window 0x%x maps", xsurface->window_id);
    geom = {xsurface->x, xsurface->y, xsurface->width, xsurface->height};
    handle_map();
    if (unmanaged())
        return;
    if (xsurface->fullscreen)
        set_fullscreen(true);
    else if (xsurface->maximized_horz && xsurface->maximized_vert)
        set_maximized(true);
    apply_states();
}

void XwaylandView::apply_states() {
    const bool was_above = keep_above, was_below = keep_below;
    keep_above = xsurface->above;
    keep_below = xsurface->below && !xsurface->above;
    sticky = xsurface->sticky;
    skip_taskbar = xsurface->skip_taskbar;
    if (xsurface->demands_attention && this != server.focused_view)
        urgent = true;
    if (keep_above != was_above || keep_below != was_below)
        raise();
    server.notify_window(*this, "changed");
}

// Where it asked to be put, through its WM_NORMAL_HINTS. Programs often set
// PPosition to 0,0 without meaning it; that one is ignored.
std::optional<std::pair<int, int>> XwaylandView::requested_position(bool& user) const {
    const xcb_size_hints_t* h = xsurface->size_hints;
    if (!h)
        return std::nullopt;
    user = h->flags & XCB_ICCCM_SIZE_HINT_US_POSITION;
    const bool program = h->flags & XCB_ICCCM_SIZE_HINT_P_POSITION;
    if (!user && !(program && (xsurface->x != 0 || xsurface->y != 0)))
        return std::nullopt;
    return std::pair{int(xsurface->x), int(xsurface->y)};
}

bool XwaylandView::splash() const {
    return has_type(WLR_XWAYLAND_NET_WM_WINDOW_TYPE_SPLASH);
}

bool XwaylandView::passive() const {
    for (auto t : {WLR_XWAYLAND_NET_WM_WINDOW_TYPE_NOTIFICATION, WLR_XWAYLAND_NET_WM_WINDOW_TYPE_TOOLTIP,
                   WLR_XWAYLAND_NET_WM_WINDOW_TYPE_DROPDOWN_MENU, WLR_XWAYLAND_NET_WM_WINDOW_TYPE_POPUP_MENU,
                   WLR_XWAYLAND_NET_WM_WINDOW_TYPE_COMBO, WLR_XWAYLAND_NET_WM_WINDOW_TYPE_DND})
        if (has_type(t))
            return true;
    return false;
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
    request_geometry({e->x, e->y - top(), e->width, e->height + top()});
}

void XwaylandView::set_geometry() {
    if (!unmanaged() || !mapped)
        return;
    geom = {xsurface->x, xsurface->y, xsurface->width, xsurface->height};
    wlr_scene_node_set_position(&tree->node, geom.x, geom.y);
}

void XwaylandView::configure(const wlr_box& frame) {
    const wlr_box box = content_box(frame);
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

// X11 windows are decorated unless they draw their own frame (Motif hints
// saying "no title"), like Steam or Chromium's own chrome.
bool XwaylandView::wants_ssd() const {
    return !unmanaged() && !splash() && !passive() &&
           !(xsurface->decorations & WLR_XWAYLAND_SURFACE_DECORATIONS_NO_TITLE);
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
