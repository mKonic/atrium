#ifdef ATRIUM_XWAYLAND
#include "xwayland_view.hpp"

#include "seat.hpp"
#include "server.hpp"

namespace atrium {

XwaylandView::XwaylandView(Server& srv, xwayland::XSurface* xs) : View(srv, Kind::X11), xsurface(xs) {
    xsurface->data = this;
    auto& c = connections_;
    auto& e = xsurface->events;

    // The surface only exists between associate and dissociate.
    wlr_log(WLR_DEBUG, "x11: window 0x%x created", xsurface->window_id);
    c.push_back(e.associate.connect([this] {
        wlr_log(WLR_DEBUG, "x11: window 0x%x has its surface (mapped: %d, buffer: %d)", xsurface->window_id,
                surface()->mapped(), surface()->buffer() != nullptr);
        map_ = surface()->events.map.connect([this] { map(); });
        unmap_ = surface()->events.unmap.connect([this] { unmap(); });
        commit_ = surface()->events.commit.connect([this] { commit(); });
        // The XWM maps an X11 surface on a commit with a buffer after this
        // point. Xwayland's Wayland and X11 sockets race, so the buffer can
        // land before the surface is paired with its window, and a window
        // that never redraws (xmessage, a splash) then never maps.
        if (!surface()->mapped() && surface()->buffer())
            surface()->map();
    }));
    c.push_back(e.dissociate.connect([this] {
        map_.disconnect();
        unmap_.disconnect();
        commit_.disconnect();
    }));
    c.push_back(e.destroy.connect([this] { delete this; }));

    c.push_back(e.request_activate.connect([this] {
        if (!unmanaged())
            xsurface->activate(true);
    }));
    c.push_back(e.request_configure.connect(
        [this](const xwayland::XSurface::ConfigureRequest& r) { request_configure(r); }));
    c.push_back(e.request_fullscreen.connect([this] {
        if (mapped)
            set_fullscreen(xsurface->fullscreen);
    }));
    c.push_back(e.request_maximize.connect([this] {
        if (mapped && layout_owned())
            send_maximized(maximized);  // stays as it is
        else if (mapped)
            set_maximized(xsurface->maximized_horz || xsurface->maximized_vert);
    }));
    c.push_back(e.request_minimize.connect([this](bool minimize) {
        if (mapped && !layout_owned() && request_minimized(minimize) != minimize)
            xsurface->set_minimized(minimized);  // it stays as it is
    }));
    c.push_back(e.request_close.connect([this] { close(); }));
    c.push_back(e.request_move.connect([this] {
        if (mapped && !layout_owned())
            server.seat->begin_move(this);
    }));
    c.push_back(e.request_resize.connect([this](uint32_t edges) {
        if (mapped && !layout_owned())
            server.seat->begin_resize(this, edges);
    }));
    c.push_back(e.set_geometry.connect([this] { set_geometry(); }));
    c.push_back(e.set_hints.connect([this] {
        if (this != server.focused_view && xsurface->hints)
            urgent = xcb_icccm_wm_hints_get_urgency(&*xsurface->hints);
    }));
    c.push_back(e.set_title.connect([this] { update_title(); }));
    c.push_back(e.set_class.connect([this] { update_title(); }));
    c.push_back(e.set_decorations.connect([this] { refresh_decoration_mode(); }));
    // A window turning into a menu (or back) while shown: taken down as what
    // it was, put up again as what it is now.
    c.push_back(e.set_override_redirect.connect([this] {
        if (!mapped)
            return;
        handle_unmap();
        map();
    }));
    auto states = [this] {
        if (mapped && !unmanaged())
            apply_states();
    };
    c.push_back(e.request_above.connect(states));
    c.push_back(e.request_below.connect(states));
    c.push_back(e.request_sticky.connect(states));
    c.push_back(e.request_skip_taskbar.connect(states));
    c.push_back(e.request_demands_attention.connect(states));
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
    const xcb_size_hints_t* h = xsurface->size_hints ? &*xsurface->size_hints : nullptr;
    if (!h)
        return std::nullopt;
    user = h->flags & XCB_ICCCM_SIZE_HINT_US_POSITION;
    const bool program = h->flags & XCB_ICCCM_SIZE_HINT_P_POSITION;
    if (!user && !(program && (xsurface->x != 0 || xsurface->y != 0)))
        return std::nullopt;
    return std::pair{int(xsurface->x), int(xsurface->y)};
}

bool XwaylandView::splash() const {
    return has_type(xwayland::WindowType::Splash);
}

bool XwaylandView::passive() const {
    using T = xwayland::WindowType;
    for (auto t : {T::Notification, T::Tooltip, T::DropdownMenu, T::PopupMenu, T::Combo, T::Dnd})
        if (has_type(t))
            return true;
    return false;
}

void XwaylandView::unmap() {
    handle_unmap();
}

void XwaylandView::commit() {
    if (mapped && !unmanaged()) {
        handle_size(surface()->current().width, surface()->current().height);
        update_corners();
    }
}

void XwaylandView::request_configure(const xwayland::XSurface::ConfigureRequest& r) {
    const auto* e = &r;
    if (!mapped || unmanaged()) {
        xsurface->configure(e->x, e->y, e->width, e->height);
        if (tree && unmanaged())
            tree->set_position(e->x, e->y);
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
    tree->set_position(geom.x, geom.y);
}

void XwaylandView::configure(const wlr_box& frame) {
    const wlr_box box = content_box(frame);
    xsurface->configure(int16_t(box.x), int16_t(box.y), uint16_t(box.width), uint16_t(box.height));
}

scene::Tree* XwaylandView::create_content(scene::Tree* parent) {
    return scene::subsurface_tree_create(parent, surface());
}

void XwaylandView::send_activated(bool a) {
    xsurface->activate(a);
    if (a)
        xsurface->restack(nullptr, XCB_STACK_MODE_ABOVE);
}

void XwaylandView::send_maximized(bool m) {
    xsurface->set_maximized(m, m);
}

void XwaylandView::send_fullscreen(bool f) {
    xsurface->set_fullscreen(f);
}

const char* XwaylandView::app_id() const {
    return xsurface->class_ ? xsurface->class_->c_str() : "";
}

const char* XwaylandView::title() const {
    return xsurface->title_or_empty();
}

View* XwaylandView::parent() const {
    return xsurface->parent ? static_cast<View*>(xsurface->parent->data) : nullptr;
}

void XwaylandView::size_hints(wlr_box& min, wlr_box& max) const {
    min = max = {};
    if (const xcb_size_hints_t* h = xsurface->size_hints ? &*xsurface->size_hints : nullptr) {
        min = {0, 0, h->min_width, h->min_height};
        max = {0, 0, h->max_width, h->max_height};
    }
}

bool XwaylandView::is_dialog() const {
    if (xsurface->modal || xsurface->parent)
        return true;
    using T = xwayland::WindowType;
    for (auto type : {T::Dialog, T::Splash, T::Toolbar, T::Utility})
        if (has_type(type))
            return true;
    wlr_box min, max;
    size_hints(min, max);
    return min.width > 0 && min.height > 0 && (min.width == max.width || min.height == max.height);
}

// X11 windows are decorated unless they draw their own frame (Motif hints
// saying "no title"), like Steam or Chromium's own chrome.
bool XwaylandView::wants_ssd() const {
    return !unmanaged() && !splash() && !passive() &&
           !(xsurface->decorations & xwayland::DecorationsNoTitle);
}

bool XwaylandView::wants_focus() const {
    return unmanaged() && xsurface->override_redirect_wants_focus() &&
           xsurface->icccm_input_model() != xwayland::InputModel::None;
}

void XwaylandView::close() {
    xsurface->close();
}

} // namespace atrium
#endif
