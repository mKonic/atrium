// The X11 window manager, ported from wlroots' xwayland/xwm.c (MIT).
#ifdef ATRIUM_XWAYLAND
#include "xwayland/xwm.hpp"

#include "wl/selection.hpp"
#include "wl/xwayland_shell.hpp"
#include "xwayland/atoms.hpp"
#include "xwayland/selection.hpp"

#include <xcb/composite.h>
#include <xcb/render.h>
#include <xcb/res.h>

#include <algorithm>
#include <cassert>
#include <cstring>
#include <poll.h>
#include <unistd.h>
#include <unordered_map>

extern "C" {
#include <wlr/util/log.h>
}

namespace atrium::xwayland {

namespace {

constexpr uint8_t kResponseTypeMask = 0x7f;

template <class T>
struct Reply {
    T* p;
    explicit Reply(T* r) : p(r) {}
    ~Reply() { free(p); }
    Reply(const Reply&) = delete;
    T* operator->() const { return p; }
    T* get() const { return p; }
    explicit operator bool() const { return p != nullptr; }
};

// Which X11 window draws each wl_surface (wlroots keeps this as an addon).
std::unordered_map<wl::Surface*, XSurface*>& paired() {
    static std::unordered_map<wl::Surface*, XSurface*> map;
    return map;
}

xcb_void_cookie_t send_event(xcb_connection_t* c, xcb_window_t dest, uint32_t mask, const void* event,
                             size_t length) {
    // xcb_send_event reads 32 bytes, whatever the event's size.
    char buf[32] = {};
    std::memcpy(buf, event, std::min<size_t>(length, 32));
    return xcb_send_event(c, 0, dest, mask, buf);
}

std::optional<std::string> string_value(xcb_get_property_reply_t* reply) {
    const int len = xcb_get_property_value_length(reply);
    if (len <= 0)
        return std::nullopt;
    return std::string(static_cast<const char*>(xcb_get_property_value(reply)), size_t(len));
}

std::vector<xcb_atom_t> atom_values(xcb_get_property_reply_t* reply) {
    const auto* atoms = static_cast<const xcb_atom_t*>(xcb_get_property_value(reply));
    const size_t n = size_t(xcb_get_property_value_length(reply)) / sizeof(xcb_atom_t);
    return {atoms, atoms + n};
}

constexpr uint32_t kEdgeTop = 1, kEdgeBottom = 2, kEdgeLeft = 4, kEdgeRight = 8;

uint32_t moveresize_edges(uint32_t detail) {
    switch (detail) {
    case 0: return kEdgeTop | kEdgeLeft;
    case 1: return kEdgeTop;
    case 2: return kEdgeTop | kEdgeRight;
    case 3: return kEdgeRight;
    case 4: return kEdgeBottom | kEdgeRight;
    case 5: return kEdgeBottom;
    case 6: return kEdgeBottom | kEdgeLeft;
    case 7: return kEdgeLeft;
    default: return 0;
    }
}

// _NET_WM_STATE's actions.
bool update_state(uint32_t action, bool& state) {
    bool next;
    switch (action) {
    case 0: next = false; break;
    case 1: next = true; break;
    case 2: next = !state; break;
    default: return false;
    }
    const bool changed = state != next;
    state = next;
    return changed;
}

} // namespace

// ---- XSurface -----------------------------------------------------------------------

XSurface::XSurface(Xwm& w, xcb_window_t window, int16_t x_, int16_t y_, uint16_t w_, uint16_t h_, bool o)
    : xwm(w), window_id(window), x(x_), y(y_), width(w_), height(h_), override_redirect(o) {
    wl_event_loop* loop = wl_display_get_event_loop(xwm.display_);
    ping_timer_ = wl_event_loop_add_timer(loop, [](void* data) {
        auto* s = static_cast<XSurface*>(data);
        s->events.ping_timeout.emit();
        s->pinging = false;
        return 1;
    }, this);
}

XSurface::~XSurface() {
    if (ping_timer_)
        wl_event_source_remove(ping_timer_);
}

XSurface* XSurface::from(wl::Surface* surface) {
    auto it = paired().find(surface);
    return it == paired().end() ? nullptr : it->second;
}

void XSurface::set_net_wm_state() {
    xcb_connection_t* c = xwm.conn_;
    // EWMH: unset while withdrawn.
    if (withdrawn) {
        xcb_delete_property(c, window_id, xwm.atom(NET_WM_STATE));
        return;
    }
    std::vector<xcb_atom_t> p;
    auto add = [&](bool on, int a) {
        if (on)
            p.push_back(xwm.atom(a));
    };
    add(modal, NET_WM_STATE_MODAL);
    add(fullscreen, NET_WM_STATE_FULLSCREEN);
    add(maximized_vert, NET_WM_STATE_MAXIMIZED_VERT);
    add(maximized_horz, NET_WM_STATE_MAXIMIZED_HORZ);
    add(minimized, NET_WM_STATE_HIDDEN);
    add(sticky, NET_WM_STATE_STICKY);
    add(shaded, NET_WM_STATE_SHADED);
    add(skip_taskbar, NET_WM_STATE_SKIP_TASKBAR);
    add(skip_pager, NET_WM_STATE_SKIP_PAGER);
    add(above, NET_WM_STATE_ABOVE);
    add(below, NET_WM_STATE_BELOW);
    add(demands_attention, NET_WM_STATE_DEMANDS_ATTENTION);
    add(this == xwm.focus_, NET_WM_STATE_FOCUSED);
    xcb_change_property(c, XCB_PROP_MODE_REPLACE, window_id, xwm.atom(NET_WM_STATE), XCB_ATOM_ATOM, 32,
                        uint32_t(p.size()), p.data());
}

void XSurface::set_wm_state() {
    uint32_t property[] = {XCB_ICCCM_WM_STATE_NORMAL, XCB_WINDOW_NONE};
    if (withdrawn)
        property[0] = XCB_ICCCM_WM_STATE_WITHDRAWN;
    else if (minimized)
        property[0] = XCB_ICCCM_WM_STATE_ICONIC;
    xcb_change_property(xwm.conn_, XCB_PROP_MODE_REPLACE, window_id, xwm.atom(WM_STATE), xwm.atom(WM_STATE), 32,
                        2, property);
}

void XSurface::dissociate() {
    if (surface) {
        wl::Surface* s = surface;
        s->unmap();
        events.dissociate.emit();
        commit_.disconnect();
        map_.disconnect();
        unmap_.disconnect();
        surface_gone_.disconnect();
        paired().erase(s);
        surface = nullptr;
    }
    std::erase(xwm.unpaired_, this);
    surface_id = 0;
    serial = 0;
    std::erase(xwm.stack_, this);
    xwm.set_net_client_list_stacking();
}

void XSurface::activate(bool activated) {
    if (activated)
        xwm.surface_activate(this);
    else if (xwm.focus_ == this)
        xwm.surface_activate(nullptr);
}

void XSurface::restack(XSurface* sibling, xcb_stack_mode_t mode) {
    assert(!override_redirect);
    // X11 clients expect their override-redirect windows to stay on top:
    // above the topmost managed window, not above everything.
    if (mode == XCB_STACK_MODE_ABOVE && !sibling && !xwm.stack_.empty())
        sibling = xwm.stack_.back();
    if (sibling == this)
        return;
    uint32_t values[2];
    size_t n = 0;
    uint32_t flags = XCB_CONFIG_WINDOW_STACK_MODE;
    if (sibling) {
        values[n++] = sibling->window_id;
        flags |= XCB_CONFIG_WINDOW_SIBLING;
    }
    values[n++] = mode;
    xcb_configure_window(xwm.conn_, window_id, uint16_t(flags), values);

    auto& st = xwm.stack_;
    std::erase(st, this);
    if (mode == XCB_STACK_MODE_ABOVE) {
        auto it = sibling ? std::ranges::find(st, sibling) : st.end();
        st.insert(it == st.end() ? st.end() : it + 1, this);
    } else if (mode == XCB_STACK_MODE_BELOW) {
        auto it = sibling ? std::ranges::find(st, sibling) : st.begin();
        st.insert(it, this);
    }
    xwm.set_net_client_list_stacking();
    xwm.schedule_flush();
}

void XSurface::configure(int16_t nx, int16_t ny, uint16_t nw, uint16_t nh) {
    const int old_w = width, old_h = height;
    x = nx;
    y = ny;
    width = nw;
    height = nh;
    const uint32_t mask = XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y | XCB_CONFIG_WINDOW_WIDTH |
                          XCB_CONFIG_WINDOW_HEIGHT | XCB_CONFIG_WINDOW_BORDER_WIDTH;
    const uint32_t values[] = {uint32_t(nx), uint32_t(ny), nw, nh, 0};
    xcb_configure_window(xwm.conn_, window_id, uint16_t(mask), values);
    // Same size: the server sends no ConfigureNotify, so a synthetic one
    // tells the window where it is (ICCCM 4.1.5). Not for override-redirect.
    if (nw == old_w && nh == old_h && !override_redirect) {
        xcb_configure_notify_event_t ev{};
        ev.response_type = XCB_CONFIGURE_NOTIFY;
        ev.event = window_id;
        ev.window = window_id;
        ev.x = nx;
        ev.y = ny;
        ev.width = nw;
        ev.height = nh;
        send_event(xwm.conn_, window_id, XCB_EVENT_MASK_STRUCTURE_NOTIFY, &ev, sizeof(ev));
    }
    xwm.schedule_flush();
}

void XSurface::close() {
    if (xwm.atoms_contain(protocols, WM_DELETE_WINDOW)) {
        xcb_client_message_data_t d{};
        d.data32[0] = xwm.atom(WM_DELETE_WINDOW);
        d.data32[1] = XCB_CURRENT_TIME;
        xwm.send_wm_message(this, d, XCB_EVENT_MASK_NO_EVENT);
    } else {
        xcb_kill_client(xwm.conn_, window_id);
        xwm.schedule_flush();
    }
}

void XSurface::set_withdrawn(bool w) {
    withdrawn = w;
    set_wm_state();
    set_net_wm_state();
    xwm.schedule_flush();
}

void XSurface::set_minimized(bool m) {
    minimized = m;
    set_wm_state();
    set_net_wm_state();
    xwm.schedule_flush();
}

void XSurface::set_maximized(bool horz, bool vert) {
    maximized_horz = horz;
    maximized_vert = vert;
    set_net_wm_state();
    xwm.schedule_flush();
}

#define ATRIUM_XSURFACE_STATE(fn, field) \
    void XSurface::fn(bool v) {          \
        field = v;                       \
        set_net_wm_state();              \
        xwm.schedule_flush();            \
    }
ATRIUM_XSURFACE_STATE(set_fullscreen, fullscreen)
ATRIUM_XSURFACE_STATE(set_sticky, sticky)
ATRIUM_XSURFACE_STATE(set_shaded, shaded)
ATRIUM_XSURFACE_STATE(set_skip_taskbar, skip_taskbar)
ATRIUM_XSURFACE_STATE(set_skip_pager, skip_pager)
ATRIUM_XSURFACE_STATE(set_above, above)
ATRIUM_XSURFACE_STATE(set_below, below)
ATRIUM_XSURFACE_STATE(set_demands_attention, demands_attention)
#undef ATRIUM_XSURFACE_STATE

void XSurface::offer_focus() {
    if (override_redirect || !xwm.atoms_contain(protocols, WM_TAKE_FOCUS))
        return;
    xwm.offered_focus_ = this;
    xcb_client_message_data_t d{};
    d.data32[0] = xwm.atom(WM_TAKE_FOCUS);
    d.data32[1] = XCB_TIME_CURRENT_TIME;
    xwm.send_wm_message(this, d, XCB_EVENT_MASK_NO_EVENT);
    xcb_flush(xwm.conn_);
}

void XSurface::ping() {
    xcb_client_message_data_t d{};
    d.data32[0] = xwm.atom(NET_WM_PING);
    d.data32[1] = XCB_CURRENT_TIME;
    d.data32[2] = window_id;
    xwm.send_wm_message(this, d, XCB_EVENT_MASK_NO_EVENT);
    wl_event_source_timer_update(ping_timer_, int(xwm.ping_timeout_ms_));
    pinging = true;
}

bool XSurface::has_window_type(WindowType type) const {
    static constexpr int kAtoms[] = {
        NET_WM_WINDOW_TYPE_DESKTOP,       NET_WM_WINDOW_TYPE_DOCK,       NET_WM_WINDOW_TYPE_TOOLBAR,
        NET_WM_WINDOW_TYPE_MENU,          NET_WM_WINDOW_TYPE_UTILITY,    NET_WM_WINDOW_TYPE_SPLASH,
        NET_WM_WINDOW_TYPE_DIALOG,        NET_WM_WINDOW_TYPE_DROPDOWN_MENU, NET_WM_WINDOW_TYPE_POPUP_MENU,
        NET_WM_WINDOW_TYPE_TOOLTIP,       NET_WM_WINDOW_TYPE_NOTIFICATION, NET_WM_WINDOW_TYPE_COMBO,
        NET_WM_WINDOW_TYPE_DND,           NET_WM_WINDOW_TYPE_NORMAL,
    };
    return xwm.atoms_contain(window_type, kAtoms[int(type)]);
}

bool XSurface::override_redirect_wants_focus() const {
    for (int a : {NET_WM_WINDOW_TYPE_COMBO, NET_WM_WINDOW_TYPE_DND, NET_WM_WINDOW_TYPE_DROPDOWN_MENU,
                  NET_WM_WINDOW_TYPE_MENU, NET_WM_WINDOW_TYPE_NOTIFICATION, NET_WM_WINDOW_TYPE_POPUP_MENU,
                  NET_WM_WINDOW_TYPE_SPLASH, NET_WM_WINDOW_TYPE_DESKTOP, NET_WM_WINDOW_TYPE_TOOLTIP,
                  NET_WM_WINDOW_TYPE_UTILITY})
        if (xwm.atoms_contain(window_type, a))
            return false;
    return true;
}

InputModel XSurface::icccm_input_model() const {
    const bool take_focus = xwm.atoms_contain(protocols, WM_TAKE_FOCUS);
    if (!hints || hints->input)
        return take_focus ? InputModel::Local : InputModel::Passive;
    return take_focus ? InputModel::Global : InputModel::None;
}

// ---- Xwm: focus, lists ----------------------------------------------------------------

bool Xwm::atoms_contain(const std::vector<xcb_atom_t>& atoms, int name) const {
    return std::ranges::find(atoms, atoms_[name]) != atoms.end();
}

XSurface* Xwm::lookup(xcb_window_t window) const {
    for (const auto& s : surfaces_)
        if (s->window_id == window)
            return s.get();
    return nullptr;
}

void Xwm::schedule_flush() {
    pollfd p{xcb_get_file_descriptor(conn_), POLLOUT, 0};
    if (poll(&p, 1, 0) < 0)
        return;
    if (p.revents & POLLOUT) {
        xcb_flush(conn_);
        return;
    }
    wl_event_source_fd_update(source_, WL_EVENT_READABLE | WL_EVENT_WRITABLE);
}

void Xwm::send_wm_message(XSurface* s, const xcb_client_message_data_t& data, uint32_t event_mask) {
    xcb_client_message_event_t ev{};
    ev.response_type = XCB_CLIENT_MESSAGE;
    ev.format = 32;
    ev.window = s->window_id;
    ev.type = atoms_[WM_PROTOCOLS];
    ev.data = data;
    send_event(conn_, s->window_id, event_mask, &ev, sizeof(ev));
    schedule_flush();
}

void Xwm::set_net_active_window(xcb_window_t window) {
    xcb_change_property(conn_, XCB_PROP_MODE_REPLACE, screen_->root, atoms_[NET_ACTIVE_WINDOW], atoms_[WINDOW],
                        32, 1, &window);
}

void Xwm::set_net_client_list() {
    std::vector<xcb_window_t> windows;
    for (const auto& s : surfaces_)
        if (s->surface && s->surface->mapped())
            windows.push_back(s->window_id);
    xcb_change_property(conn_, XCB_PROP_MODE_REPLACE, screen_->root, atoms_[NET_CLIENT_LIST], XCB_ATOM_WINDOW, 32,
                        uint32_t(windows.size()), windows.data());
}

void Xwm::set_net_client_list_stacking() {
    std::vector<xcb_window_t> windows;
    for (XSurface* s : stack_)
        windows.push_back(s->window_id);
    xcb_change_property(conn_, XCB_PROP_MODE_REPLACE, screen_->root, atoms_[NET_CLIENT_LIST_STACKING],
                        XCB_ATOM_WINDOW, 32, uint32_t(windows.size()), windows.data());
}

// Gives the keyboard to a window (null: to none).
void Xwm::focus_window(XSurface* s) {
    if (!s) {
        // PointerRoot (1), not None: None turns the keyboard off, which
        // breaks grabs for popups.
        xcb_set_input_focus_checked(conn_, XCB_INPUT_FOCUS_POINTER_ROOT, 1, XCB_CURRENT_TIME);
        return;
    }
    if (s->override_redirect)
        return;
    xcb_client_message_data_t d{};
    d.data32[0] = atoms_[WM_TAKE_FOCUS];
    d.data32[1] = XCB_TIME_CURRENT_TIME;
    if (s->hints && !s->hints->input) {
        // It takes focus itself, when told it may.
        send_wm_message(s, d, XCB_EVENT_MASK_NO_EVENT);
    } else {
        send_wm_message(s, d, XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT);
        const xcb_void_cookie_t cookie =
            xcb_set_input_focus(conn_, XCB_INPUT_FOCUS_POINTER_ROOT, s->window_id, XCB_CURRENT_TIME);
        last_focus_seq_ = uint16_t(cookie.sequence);
    }
}

// _NET_ACTIVE_WINDOW and _NET_WM_STATE_FOCUSED follow focus.
void Xwm::set_focused_window(XSurface* s) {
    XSurface* old = focus_;
    if (s && s->override_redirect)
        return;
    focus_ = s;
    offered_focus_ = s;  // an offer is answered
    if (s == old)
        return;
    if (old)
        old->set_net_wm_state();
    if (s) {
        s->set_net_wm_state();
        set_net_active_window(s->window_id);
    } else {
        set_net_active_window(no_focus_window_);
    }
}

void Xwm::surface_activate(XSurface* s) {
    if (s && s->override_redirect)
        return;
    if (s != focus_ && s != offered_focus_)
        focus_window(s);
    set_focused_window(s);
    schedule_flush();
}

void Xwm::update_override_redirect(XSurface* s, bool o) {
    if (s->override_redirect == o)
        return;
    s->override_redirect = o;
    if (o) {
        std::erase(stack_, s);
        set_net_client_list_stacking();
    } else if (s->surface && s->surface->mapped()) {
        s->restack(nullptr, XCB_STACK_MODE_BELOW);
    }
    s->events.set_override_redirect.emit();
}

std::string Xwm::atom_name(xcb_atom_t atom) {
    Reply<xcb_get_atom_name_reply_t> r(xcb_get_atom_name_reply(conn_, xcb_get_atom_name(conn_, atom), nullptr));
    if (!r)
        return {};
    return std::string(xcb_get_atom_name_name(r.get()), size_t(xcb_get_atom_name_name_length(r.get())));
}

// ---- properties ----------------------------------------------------------------------

namespace {
constexpr uint32_t kMwmHintsDecorations = 1 << 1;
constexpr uint32_t kMwmDecorAll = 1 << 0, kMwmDecorBorder = 1 << 1, kMwmDecorTitle = 1 << 3;
} // namespace

void Xwm::read_property(XSurface* s, xcb_atom_t property, xcb_get_property_reply_t* reply) {
    const bool stringy = reply->type == XCB_ATOM_STRING || reply->type == atoms_[UTF8_STRING] ||
                         reply->type == XCB_ATOM_NONE;
    if (property == XCB_ATOM_WM_CLASS) {
        if (!stringy)
            return;
        // Two strings one after the other: instance, class.
        const auto v = string_value(reply);
        s->instance.reset();
        s->class_.reset();
        if (v) {
            const size_t nul = v->find('\0');
            if (nul != std::string::npos) {
                s->instance = v->substr(0, nul);
                std::string rest = v->substr(nul + 1);
                if (auto end = rest.find('\0'); end != std::string::npos)
                    rest.resize(end);
                if (!rest.empty())
                    s->class_ = rest;
            } else {
                s->class_ = *v;
            }
        }
        s->events.set_class.emit();
    } else if (property == XCB_ATOM_WM_NAME || property == atoms_[NET_WM_NAME]) {
        if (!stringy)
            return;
        (property == XCB_ATOM_WM_NAME ? s->wm_name_ : s->net_wm_name_) = string_value(reply);
        s->title = s->net_wm_name_ ? s->net_wm_name_ : s->wm_name_;
        s->events.set_title.emit();
    } else if (property == XCB_ATOM_WM_TRANSIENT_FOR) {
        if (reply->type != XCB_ATOM_WINDOW && reply->type != XCB_ATOM_NONE)
            return;
        XSurface* found = nullptr;
        if (reply->type != XCB_ATOM_NONE) {
            if (xcb_get_property_value_length(reply) != sizeof(xcb_window_t))
                return;
            found = lookup(*static_cast<const xcb_window_t*>(xcb_get_property_value(reply)));
            // Not one that would make a loop.
            for (XSurface* p = found; p; p = p->parent)
                if (p == s) {
                    found = nullptr;
                    break;
                }
        }
        if (s->parent)
            std::erase(s->parent->children, s);
        s->parent = found;
        if (found)
            found->children.push_back(s);
        s->events.set_parent.emit();
    } else if (property == atoms_[NET_WM_WINDOW_TYPE]) {
        if (reply->type != XCB_ATOM_ATOM && reply->type != XCB_ATOM_NONE)
            return;
        s->window_type = atom_values(reply);
        s->events.set_window_type.emit();
    } else if (property == atoms_[NET_WM_ICON]) {
        s->events.set_icon.emit();
    } else if (property == atoms_[WM_PROTOCOLS]) {
        if (reply->type != XCB_ATOM_ATOM && reply->type != XCB_ATOM_NONE)
            return;
        s->protocols = atom_values(reply);
    } else if (property == atoms_[NET_WM_STATE]) {
        s->fullscreen = false;
        for (xcb_atom_t a : atom_values(reply)) {
            if (a == atoms_[NET_WM_STATE_MODAL]) s->modal = true;
            else if (a == atoms_[NET_WM_STATE_FULLSCREEN]) s->fullscreen = true;
            else if (a == atoms_[NET_WM_STATE_MAXIMIZED_VERT]) s->maximized_vert = true;
            else if (a == atoms_[NET_WM_STATE_MAXIMIZED_HORZ]) s->maximized_horz = true;
            else if (a == atoms_[NET_WM_STATE_HIDDEN]) s->minimized = true;
            else if (a == atoms_[NET_WM_STATE_STICKY]) s->sticky = true;
            else if (a == atoms_[NET_WM_STATE_SHADED]) s->shaded = true;
            else if (a == atoms_[NET_WM_STATE_SKIP_TASKBAR]) s->skip_taskbar = true;
            else if (a == atoms_[NET_WM_STATE_SKIP_PAGER]) s->skip_pager = true;
            else if (a == atoms_[NET_WM_STATE_ABOVE]) s->above = true;
            else if (a == atoms_[NET_WM_STATE_BELOW]) s->below = true;
            else if (a == atoms_[NET_WM_STATE_DEMANDS_ATTENTION]) s->demands_attention = true;
        }
    } else if (property == atoms_[WM_HINTS]) {
        // ICCCM says WM_HINTS; in practice it is often ATOM.
        if (reply->type != atoms_[WM_HINTS] && reply->type != XCB_ATOM_ATOM && reply->type != XCB_ATOM_NONE)
            return;
        s->hints.reset();
        if (xcb_get_property_value_length(reply) > 0) {
            xcb_icccm_wm_hints_t h{};
            xcb_icccm_get_wm_hints_from_reply(&h, reply);
            if (!(h.flags & XCB_ICCCM_WM_HINT_INPUT))
                h.input = true;  // unsaid: it wants input
            s->hints = h;
        }
        s->events.set_hints.emit();
    } else if (property == atoms_[WM_NORMAL_HINTS]) {
        if (reply->type != atoms_[WM_SIZE_HINTS] && reply->type != XCB_ATOM_NONE)
            return;
        s->size_hints.reset();
        if (xcb_get_property_value_length(reply) == 0)
            return;
        xcb_size_hints_t h{};
        xcb_icccm_get_wm_size_hints_from_reply(&h, reply);
        const bool has_min = h.flags & XCB_ICCCM_SIZE_HINT_P_MIN_SIZE;
        const bool has_base = h.flags & XCB_ICCCM_SIZE_HINT_BASE_SIZE;
        // ICCCM: either stands for the other when absent.
        if (!has_min && !has_base) {
            h.min_width = h.min_height = h.base_width = h.base_height = -1;
        } else if (!has_base) {
            h.base_width = h.min_width;
            h.base_height = h.min_height;
        } else if (!has_min) {
            h.min_width = h.base_width;
            h.min_height = h.base_height;
        }
        if (!(h.flags & XCB_ICCCM_SIZE_HINT_P_MAX_SIZE))
            h.max_width = h.max_height = -1;
        s->size_hints = h;
        s->events.set_size_hints.emit();
    } else if (property == atoms_[MOTIF_WM_HINTS]) {
        const int n = xcb_get_property_value_length(reply) / int(sizeof(uint32_t));
        if (n == 0) {
            s->decorations = DecorationsAll;
            s->events.set_decorations.emit();
            return;
        }
        if (n < 5)
            return;
        const auto* m = static_cast<const uint32_t*>(xcb_get_property_value(reply));
        if (m[0] & kMwmHintsDecorations) {
            s->decorations = DecorationsAll;
            if (!(m[2] & kMwmDecorAll)) {
                if (!(m[2] & kMwmDecorBorder))
                    s->decorations |= DecorationsNoBorder;
                if (!(m[2] & kMwmDecorTitle))
                    s->decorations |= DecorationsNoTitle;
            }
            s->events.set_decorations.emit();
        }
    } else if (property == atoms_[NET_WM_STRUT_PARTIAL]) {
        s->strut_partial.reset();
        if (reply->type != XCB_ATOM_NONE) {
            if (reply->type != XCB_ATOM_CARDINAL || reply->format != 32 ||
                size_t(xcb_get_property_value_length(reply)) != sizeof(xcb_ewmh_wm_strut_partial_t))
                return;
            xcb_ewmh_wm_strut_partial_t sp{};
            std::memcpy(&sp, xcb_get_property_value(reply), sizeof(sp));
            s->strut_partial = sp;
        }
        s->events.set_strut_partial.emit();
    } else if (property == atoms_[WM_WINDOW_ROLE]) {
        if (!stringy)
            return;
        s->role = string_value(reply);
        s->events.set_role.emit();
    } else if (property == atoms_[NET_STARTUP_ID]) {
        if (!stringy)
            return;
        s->startup_id = string_value(reply);
        s->events.set_startup_id.emit();
    } else if (property == atoms_[NET_WM_WINDOW_OPACITY]) {
        if (reply->type == XCB_ATOM_NONE) {
            s->opacity = 1;
        } else {
            if (reply->type != XCB_ATOM_CARDINAL || reply->format != 32 ||
                xcb_get_property_value_length(reply) != sizeof(uint32_t))
                return;
            s->opacity = float(double(*static_cast<const uint32_t*>(xcb_get_property_value(reply))) / UINT32_MAX);
        }
        s->events.set_opacity.emit();
    }
}

namespace {
xcb_get_property_cookie_t get_property(xcb_connection_t* c, xcb_window_t w, xcb_atom_t atom, xcb_atom_t icon) {
    // The icon is the compositor's to fetch whole when it wants it.
    return xcb_get_property(c, 0, w, atom, XCB_ATOM_ANY, 0, atom == icon ? 0 : 2048);
}
} // namespace

void Xwm::associate(XSurface* s, wl::Surface* surface) {
    assert(!s->surface);
    std::erase(unpaired_, s);
    s->surface_id = 0;
    s->surface = surface;
    paired()[surface] = s;
    // Mapped on its first buffer.
    s->commit_ = surface->events.commit.connect([surface] {
        if (surface->buffer())
            surface->map();
    });
    s->map_ = surface->events.map.connect([this] { set_net_client_list(); });
    s->unmap_ = surface->events.unmap.connect([this] { set_net_client_list(); });
    s->surface_gone_ = surface->events.destroy.connect([s] { s->dissociate(); });

    const xcb_atom_t props[] = {
        XCB_ATOM_WM_CLASS,         XCB_ATOM_WM_NAME,         XCB_ATOM_WM_TRANSIENT_FOR,
        atoms_[WM_PROTOCOLS],      atoms_[WM_HINTS],         atoms_[WM_NORMAL_HINTS],
        atoms_[MOTIF_WM_HINTS],    atoms_[NET_STARTUP_ID],   atoms_[NET_WM_STATE],
        atoms_[NET_WM_STRUT_PARTIAL], atoms_[NET_WM_WINDOW_TYPE], atoms_[NET_WM_NAME],
        atoms_[NET_WM_ICON],
    };
    xcb_get_property_cookie_t cookies[std::size(props)];
    for (size_t i = 0; i < std::size(props); ++i)
        cookies[i] = get_property(conn_, s->window_id, props[i], atoms_[NET_WM_ICON]);
    for (size_t i = 0; i < std::size(props); ++i) {
        Reply<xcb_get_property_reply_t> r(xcb_get_property_reply(conn_, cookies[i], nullptr));
        if (r)
            read_property(s, props[i], r.get());
    }
    s->events.associate.emit();
}

void Xwm::destroy_surface(XSurface* s) {
    s->dissociate();
    s->events.destroy.emit();
    if (s == focus_)
        surface_activate(nullptr);
    if (s == offered_focus_)
        offered_focus_ = nullptr;
    if (s == drag_focus_)
        set_drag_focus(nullptr);
    if (s == drop_focus_)
        drop_focus_ = nullptr;
    if (s->parent)
        std::erase(s->parent->children, s);
    for (XSurface* c : s->children) {
        c->parent = nullptr;
        c->events.set_parent.emit();
    }
    std::erase_if(surfaces_, [s](const auto& p) { return p.get() == s; });
}

// ---- events ---------------------------------------------------------------------------

void Xwm::handle_create(xcb_create_notify_event_t* ev) {
    if (ev->window == window_ || selection_window(ev->window))
        return;
    const xcb_get_geometry_cookie_t geometry = xcb_get_geometry(conn_, ev->window);
    xcb_res_query_client_ids_cookie_t ids{};
    if (xres_) {
        xcb_res_client_id_spec_t spec{ev->window, XCB_RES_CLIENT_ID_MASK_LOCAL_CLIENT_PID};
        ids = xcb_res_query_client_ids(conn_, 1, &spec);
    }
    const uint32_t mask = XCB_EVENT_MASK_FOCUS_CHANGE | XCB_EVENT_MASK_PROPERTY_CHANGE;
    xcb_change_window_attributes(conn_, ev->window, XCB_CW_EVENT_MASK, &mask);

    auto s = std::make_unique<XSurface>(*this, ev->window, ev->x, ev->y, ev->width, ev->height,
                                        ev->override_redirect);
    if (Reply<xcb_get_geometry_reply_t> g(xcb_get_geometry_reply(conn_, geometry, nullptr)); g)
        s->has_alpha = g->depth == 32;
    if (xres_) {
        Reply<xcb_res_query_client_ids_reply_t> r(xcb_res_query_client_ids_reply(conn_, ids, nullptr));
        if (r) {
            for (auto it = xcb_res_query_client_ids_ids_iterator(r.get()); it.rem > 0;
                 xcb_res_client_id_value_next(&it)) {
                if ((it.data->spec.mask & XCB_RES_CLIENT_ID_MASK_LOCAL_CLIENT_PID) &&
                    xcb_res_client_id_value_value_length(it.data) > 0) {
                    s->pid = pid_t(*xcb_res_client_id_value_value(it.data));
                    break;
                }
            }
        }
    }
    XSurface* raw = s.get();
    // Newest first, as wlroots' list.
    surfaces_.insert(surfaces_.begin(), std::move(s));
    events.new_surface.emit(raw);
}

void Xwm::handle_destroy(xcb_destroy_notify_event_t* ev) {
    if (XSurface* s = lookup(ev->window)) {
        destroy_surface(s);
        handle_selection_destroy_notify(ev);
    }
}

void Xwm::handle_configure_request(xcb_configure_request_event_t* ev) {
    XSurface* s = lookup(ev->window);
    if (!s)
        return;
    const uint16_t mask = ev->value_mask;
    constexpr uint16_t geo = XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y | XCB_CONFIG_WINDOW_WIDTH |
                             XCB_CONFIG_WINDOW_HEIGHT;
    if (!(mask & geo))
        return;
    const XSurface::ConfigureRequest req{
        mask & XCB_CONFIG_WINDOW_X ? ev->x : s->x,
        mask & XCB_CONFIG_WINDOW_Y ? ev->y : s->y,
        mask & XCB_CONFIG_WINDOW_WIDTH ? ev->width : s->width,
        mask & XCB_CONFIG_WINDOW_HEIGHT ? ev->height : s->height,
        mask,
    };
    s->events.request_configure.emit(req);
}

void Xwm::handle_configure_notify(xcb_configure_notify_event_t* ev) {
    XSurface* s = lookup(ev->window);
    if (!s)
        return;
    const bool changed = s->x != ev->x || s->y != ev->y || s->width != ev->width || s->height != ev->height;
    if (changed) {
        s->x = ev->x;
        s->y = ev->y;
        s->width = ev->width;
        s->height = ev->height;
    }
    update_override_redirect(s, ev->override_redirect);
    if (changed)
        s->events.set_geometry.emit();
}

void Xwm::handle_map_request(xcb_map_request_event_t* ev) {
    if (XSurface* s = lookup(ev->window)) {
        s->events.map_request.emit();
        xcb_map_window(conn_, ev->window);
    }
}

void Xwm::handle_map_notify(xcb_map_notify_event_t* ev) {
    XSurface* s = lookup(ev->window);
    if (!s)
        return;
    update_override_redirect(s, ev->override_redirect);
    if (!s->override_redirect) {
        s->set_withdrawn(false);
        s->restack(nullptr, XCB_STACK_MODE_BELOW);
    }
}

void Xwm::handle_unmap_notify(xcb_unmap_notify_event_t* ev) {
    XSurface* s = lookup(ev->window);
    if (!s)
        return;
    s->dissociate();
    if (!s->override_redirect)
        s->set_withdrawn(true);
}

void Xwm::handle_property_notify(xcb_property_notify_event_t* ev) {
    XSurface* s = lookup(ev->window);
    if (!s)
        return;
    Reply<xcb_get_property_reply_t> r(
        xcb_get_property_reply(conn_, get_property(conn_, s->window_id, ev->atom, atoms_[NET_WM_ICON]), nullptr));
    if (r)
        read_property(s, ev->atom, r.get());
}

void Xwm::handle_net_wm_state(xcb_client_message_event_t* ev) {
    XSurface* s = lookup(ev->window);
    if (!s || ev->format != 32)
        return;
    struct Flag {
        int atom;
        bool XSurface::*field;
    };
    static constexpr Flag kFlags[] = {
        {NET_WM_STATE_MODAL, &XSurface::modal},
        {NET_WM_STATE_FULLSCREEN, &XSurface::fullscreen},
        {NET_WM_STATE_MAXIMIZED_VERT, &XSurface::maximized_vert},
        {NET_WM_STATE_MAXIMIZED_HORZ, &XSurface::maximized_horz},
        {NET_WM_STATE_HIDDEN, &XSurface::minimized},
        {NET_WM_STATE_STICKY, &XSurface::sticky},
        {NET_WM_STATE_SHADED, &XSurface::shaded},
        {NET_WM_STATE_SKIP_TASKBAR, &XSurface::skip_taskbar},
        {NET_WM_STATE_SKIP_PAGER, &XSurface::skip_pager},
        {NET_WM_STATE_ABOVE, &XSurface::above},
        {NET_WM_STATE_BELOW, &XSurface::below},
        {NET_WM_STATE_DEMANDS_ATTENTION, &XSurface::demands_attention},
    };
    bool before[std::size(kFlags)];
    for (size_t i = 0; i < std::size(kFlags); ++i)
        before[i] = s->*kFlags[i].field;

    const uint32_t action = ev->data.data32[0];
    for (int i = 0; i < 2; ++i) {
        const xcb_atom_t property = ev->data.data32[1 + i];
        for (const Flag& f : kFlags)
            if (property == atoms_[f.atom] && update_state(action, s->*f.field))
                s->set_net_wm_state();
    }
    auto changed = [&](int atom) {
        for (size_t i = 0; i < std::size(kFlags); ++i)
            if (kFlags[i].atom == atom)
                return before[i] != s->*kFlags[i].field;
        return false;
    };
    if (changed(NET_WM_STATE_FULLSCREEN))
        s->events.request_fullscreen.emit();
    if (changed(NET_WM_STATE_MAXIMIZED_VERT) || changed(NET_WM_STATE_MAXIMIZED_HORZ))
        s->events.request_maximize.emit();
    if (changed(NET_WM_STATE_HIDDEN))
        s->events.request_minimize.emit(s->minimized);
    if (changed(NET_WM_STATE_STICKY))
        s->events.request_sticky.emit();
    if (changed(NET_WM_STATE_SHADED))
        s->events.request_shaded.emit();
    if (changed(NET_WM_STATE_SKIP_TASKBAR))
        s->events.request_skip_taskbar.emit();
    if (changed(NET_WM_STATE_SKIP_PAGER))
        s->events.request_skip_pager.emit();
    if (changed(NET_WM_STATE_ABOVE))
        s->events.request_above.emit();
    if (changed(NET_WM_STATE_BELOW))
        s->events.request_below.emit();
    if (changed(NET_WM_STATE_DEMANDS_ATTENTION))
        s->events.request_demands_attention.emit();
}

void Xwm::handle_startup_info(xcb_client_message_event_t* ev) {
    auto it = std::ranges::find_if(pending_startup_, [ev](const PendingStartup& p) { return p.window == ev->window; });
    if (it == pending_startup_.end()) {
        pending_startup_.push_back({ev->window, {}});
        it = pending_startup_.end() - 1;
    }
    // The message comes 20 bytes at a time, up to its NUL.
    const auto* data = reinterpret_cast<const char*>(ev->data.data8);
    for (size_t i = 0; i < sizeof(ev->data); ++i) {
        if (data[i] != '\0') {
            it->msg.push_back(data[i]);
            continue;
        }
        static constexpr std::string_view kRemove = "remove: ID=";
        if (it->msg.starts_with(kRemove) && it->msg.size() > kRemove.size()) {
            const std::string id = it->msg.substr(kRemove.size());
            const RemoveStartupInfo info{id.c_str(), ev->window};
            pending_startup_.erase(it);
            events.remove_startup_info.emit(info);
        } else {
            pending_startup_.erase(it);
        }
        return;
    }
}

void Xwm::handle_client_message(xcb_client_message_event_t* ev) {
    const xcb_atom_t t = ev->type;
    if (t == atoms_[WL_SURFACE_ID]) {
        XSurface* s = lookup(ev->window);
        if (!s || s->surface)
            return;
        const uint32_t id = ev->data.data32[0];
        // The X11 and Wayland sockets race: the surface may come later.
        wl_resource* r = wl_client_get_object(client_, id);
        wl::Surface* surface = r ? wl::Surface::from(r) : nullptr;
        if (surface) {
            associate(s, surface);
        } else if (!r) {
            s->surface_id = id;
            std::erase(unpaired_, s);
            unpaired_.push_back(s);
        }
    } else if (t == atoms_[WL_SURFACE_SERIAL]) {
        XSurface* s = lookup(ev->window);
        if (!s || s->serial)
            return;
        s->serial = uint64_t(ev->data.data32[1]) << 32 | ev->data.data32[0];
        if (wl::Surface* surface = shell_ ? shell_->surface_from_serial(s->serial) : nullptr) {
            associate(s, surface);
        } else {
            std::erase(unpaired_, s);
            unpaired_.push_back(s);
        }
    } else if (t == atoms_[NET_WM_STATE]) {
        handle_net_wm_state(ev);
    } else if (t == atoms_[NET_WM_MOVERESIZE]) {
        XSurface* s = lookup(ev->window);
        if (!s)
            return;
        const uint32_t detail = ev->data.data32[2];
        if (detail == 8)
            s->events.request_move.emit();
        else if (detail <= 7)
            s->events.request_resize.emit(moveresize_edges(detail));
    } else if (t == atoms_[WM_PROTOCOLS]) {
        if (ev->data.data32[0] != atoms_[NET_WM_PING])
            return;
        XSurface* s = lookup(ev->data.data32[2]);
        if (!s || !s->pinging)
            return;
        wl_event_source_timer_update(s->ping_timer_, 0);
        s->pinging = false;
    } else if (t == atoms_[NET_ACTIVE_WINDOW]) {
        if (XSurface* s = lookup(ev->window))
            s->events.request_activate.emit();
    } else if (t == atoms_[NET_CLOSE_WINDOW]) {
        if (XSurface* s = lookup(ev->window))
            s->events.request_close.emit();
    } else if (t == atoms_[NET_STARTUP_INFO] || t == atoms_[NET_STARTUP_INFO_BEGIN]) {
        handle_startup_info(ev);
    } else if (t == atoms_[WM_CHANGE_STATE]) {
        XSurface* s = lookup(ev->window);
        const uint32_t detail = ev->data.data32[0];
        if (!s)
            return;
        if (detail == XCB_ICCCM_WM_STATE_ICONIC)
            s->events.request_minimize.emit(true);
        else if (detail == XCB_ICCCM_WM_STATE_NORMAL)
            s->events.request_minimize.emit(false);
    } else {
        handle_selection_client_message(ev);
    }
}

void Xwm::handle_focus_in(xcb_focus_in_event_t* ev) {
    if (ev->detail == XCB_NOTIFY_DETAIL_POINTER)
        return;
    // Grabs (popup menus taking the keyboard) are left alone, but told.
    XSurface* s = lookup(ev->event);
    if (ev->mode == XCB_NOTIFY_MODE_GRAB) {
        if (s)
            s->events.grab_focus.emit();
        return;
    }
    if (ev->mode == XCB_NOTIFY_MODE_UNGRAB)
        return;
    // Older than the last focus change atrium made: a race, ignored.
    const uint16_t dist = uint16_t(ev->sequence - last_focus_seq_);
    if (dist >= UINT16_MAX / 2)
        return;
    // Focus may move within an application (Steam needs it); not to another.
    if (s && ((focus_ && s->pid == focus_->pid) || (offered_focus_ && s->pid == offered_focus_->pid))) {
        set_focused_window(s);
        s->events.focus_in.emit();
    } else {
        focus_window(focus_);
    }
}

int Xwm::read_events() {
    int count = 0;
    while (xcb_generic_event_t* ev = xcb_poll_for_event(conn_)) {
        ++count;
        if (handle_selection_event(ev)) {
            free(ev);
            continue;
        }
        switch (ev->response_type & kResponseTypeMask) {
        case XCB_CREATE_NOTIFY: handle_create(reinterpret_cast<xcb_create_notify_event_t*>(ev)); break;
        case XCB_DESTROY_NOTIFY: handle_destroy(reinterpret_cast<xcb_destroy_notify_event_t*>(ev)); break;
        case XCB_CONFIGURE_REQUEST:
            handle_configure_request(reinterpret_cast<xcb_configure_request_event_t*>(ev));
            break;
        case XCB_CONFIGURE_NOTIFY:
            handle_configure_notify(reinterpret_cast<xcb_configure_notify_event_t*>(ev));
            break;
        case XCB_MAP_REQUEST: handle_map_request(reinterpret_cast<xcb_map_request_event_t*>(ev)); break;
        case XCB_MAP_NOTIFY: handle_map_notify(reinterpret_cast<xcb_map_notify_event_t*>(ev)); break;
        case XCB_UNMAP_NOTIFY: handle_unmap_notify(reinterpret_cast<xcb_unmap_notify_event_t*>(ev)); break;
        case XCB_PROPERTY_NOTIFY:
            handle_property_notify(reinterpret_cast<xcb_property_notify_event_t*>(ev));
            break;
        case XCB_CLIENT_MESSAGE:
            handle_client_message(reinterpret_cast<xcb_client_message_event_t*>(ev));
            break;
        case XCB_FOCUS_IN: handle_focus_in(reinterpret_cast<xcb_focus_in_event_t*>(ev)); break;
        case 0: {
            auto* e = reinterpret_cast<xcb_value_error_t*>(ev);
            wlr_log(WLR_DEBUG, "xwm: X error: op %u:%u, code %u, sequence %u, value %u", e->major_opcode,
                    e->minor_opcode, e->error_code, e->sequence, e->bad_value);
            break;
        }
        default: break;
        }
        free(ev);
    }
    return count;
}

int Xwm::on_event(uint32_t mask) {
    if (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) {
        wl_event_source_remove(source_);
        source_ = nullptr;
        events.hangup.emit();  // may delete this
        return 0;
    }
    int count = 0;
    if (mask & WL_EVENT_READABLE) {
        count = read_events();
        if (count)
            schedule_flush();
    }
    if (mask & WL_EVENT_WRITABLE) {
        // xcb_flush blocks until everything is written; it's all there is.
        xcb_flush(conn_);
        wl_event_source_fd_update(source_, WL_EVENT_READABLE);
    }
    return count;
}

// ---- setup --------------------------------------------------------------------------------

Xwm::Xwm(wl_display* display, int wm_fd, wl_client* client, wl::Compositor& compositor, wl::XwaylandShell* shell,
         bool terminate_when_idle)
    : display_(display), client_(client), shell_(shell), atoms_(ATOM_LAST, XCB_ATOM_NONE) {
    // xcb_connect_to_fd owns the fd whatever happens.
    conn_ = xcb_connect_to_fd(wm_fd, nullptr);
    if (const int err = xcb_connection_has_error(conn_)) {
        wlr_log(WLR_ERROR, "xwm: xcb connect failed: %d", err);
        xcb_disconnect(conn_);
        conn_ = nullptr;
        return;
    }
    screen_ = xcb_setup_roots_iterator(xcb_get_setup(conn_)).data;
    wl_event_loop* loop = wl_display_get_event_loop(display);
    source_ = wl_event_loop_add_fd(loop, wm_fd, WL_EVENT_READABLE,
                                   [](int, uint32_t mask, void* data) { return static_cast<Xwm*>(data)->on_event(mask); },
                                   this);
    wl_event_source_check(source_);

    // Atoms and extensions.
    xcb_prefetch_extension_data(conn_, &xcb_xfixes_id);
    xcb_prefetch_extension_data(conn_, &xcb_composite_id);
    xcb_prefetch_extension_data(conn_, &xcb_res_id);
    xcb_intern_atom_cookie_t cookies[ATOM_LAST];
    for (int i = 0; i < ATOM_LAST; ++i)
        cookies[i] = xcb_intern_atom(conn_, 0, uint16_t(std::strlen(kAtomNames[i])), kAtomNames[i]);
    for (int i = 0; i < ATOM_LAST; ++i) {
        xcb_generic_error_t* error = nullptr;
        Reply<xcb_intern_atom_reply_t> r(xcb_intern_atom_reply(conn_, cookies[i], &error));
        if (r && !error)
            atoms_[i] = r->atom;
        free(error);
    }
    xfixes_ = xcb_get_extension_data(conn_, &xcb_xfixes_id);
    if (xfixes_ && xfixes_->present) {
        Reply<xcb_xfixes_query_version_reply_t> v(xcb_xfixes_query_version_reply(
            conn_, xcb_xfixes_query_version(conn_, XCB_XFIXES_MAJOR_VERSION, XCB_XFIXES_MINOR_VERSION), nullptr));
        if (v)
            xfixes_major_ = v->major_version;
    }
    if (const auto* xres = xcb_get_extension_data(conn_, &xcb_res_id); xres && xres->present) {
        Reply<xcb_res_query_version_reply_t> v(xcb_res_query_version_reply(
            conn_, xcb_res_query_version(conn_, XCB_RES_MAJOR_VERSION, XCB_RES_MINOR_VERSION), nullptr));
        if (v && (v->server_major > 1 || (v->server_major == 1 && v->server_minor >= 2)))
            xres_ = xres;
    }

    // A 32-bit visual and colormap, and the render format cursors need.
    for (auto d = xcb_screen_allowed_depths_iterator(screen_); d.rem > 0; xcb_depth_next(&d)) {
        if (d.data->depth != 32)
            continue;
        visual_id_ = xcb_depth_visuals_iterator(d.data).data->visual_id;
        colormap_ = xcb_generate_id(conn_);
        xcb_create_colormap(conn_, XCB_COLORMAP_ALLOC_NONE, colormap_, screen_->root, visual_id_);
        break;
    }
    if (Reply<xcb_render_query_pict_formats_reply_t> f(
            xcb_render_query_pict_formats_reply(conn_, xcb_render_query_pict_formats(conn_), nullptr));
        f) {
        for (auto it = xcb_render_query_pict_formats_formats_iterator(f.get()); it.rem > 0;
             xcb_render_pictforminfo_next(&it))
            if (it.data->depth == 32) {
                render_format_id_ = it.data->id;
                break;
            }
    }

    const uint32_t root_mask = XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY | XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT |
                               XCB_EVENT_MASK_PROPERTY_CHANGE;
    xcb_change_window_attributes(conn_, screen_->root, XCB_CW_EVENT_MASK, &root_mask);
    xcb_composite_redirect_subwindows(conn_, screen_->root, XCB_COMPOSITE_REDIRECT_MANUAL);

    const xcb_atom_t supported[] = {
        atoms_[NET_WM_STATE],
        atoms_[NET_ACTIVE_WINDOW],
        atoms_[NET_CLOSE_WINDOW],
        atoms_[NET_WM_MOVERESIZE],
        atoms_[NET_WM_STATE_FOCUSED],
        atoms_[NET_WM_STATE_MODAL],
        atoms_[NET_WM_STATE_FULLSCREEN],
        atoms_[NET_WM_STATE_MAXIMIZED_VERT],
        atoms_[NET_WM_STATE_MAXIMIZED_HORZ],
        atoms_[NET_WM_STATE_HIDDEN],
        atoms_[NET_WM_STATE_STICKY],
        atoms_[NET_WM_STATE_SHADED],
        atoms_[NET_WM_STATE_SKIP_TASKBAR],
        atoms_[NET_WM_STATE_SKIP_PAGER],
        atoms_[NET_WM_STATE_ABOVE],
        atoms_[NET_WM_STATE_BELOW],
        atoms_[NET_WM_STATE_DEMANDS_ATTENTION],
        atoms_[NET_CLIENT_LIST],
        atoms_[NET_CLIENT_LIST_STACKING],
    };
    xcb_change_property(conn_, XCB_PROP_MODE_REPLACE, screen_->root, atoms_[NET_SUPPORTED], XCB_ATOM_ATOM, 32,
                        uint32_t(std::size(supported)), supported);
    // Xwayland goes when its last client does.
    if (terminate_when_idle && xfixes_major_ >= 6)
        xcb_xfixes_set_client_disconnect_mode(conn_, XCB_XFIXES_CLIENT_DISCONNECT_FLAGS_TERMINATE);
    xcb_flush(conn_);
    set_net_active_window(XCB_WINDOW_NONE);

    init_selections();

    // Surfaces Xwayland made before saying which window they draw.
    new_surface_ = compositor.new_surface.connect([this](wl::Surface* surface) {
        if (surface->client() != client_)
            return;
        const uint32_t id = wl_resource_get_id(surface->resource());
        for (XSurface* s : unpaired_)
            if (s->surface_id == id) {
                associate(s, surface);
                schedule_flush();
                return;
            }
    });
    if (shell)
        shell_surface_ = shell->new_surface.connect([this](wl::XwaylandSurface* xs) {
        for (XSurface* s : unpaired_)
            if (s->serial == xs->serial() && xs->surface()) {
                associate(s, xs->surface());
                return;
            }
    });

    // The WM's own window, and the one focus goes to when no window has it.
    static constexpr char kName[] = "atrium wm";
    window_ = xcb_generate_id(conn_);
    xcb_create_window(conn_, XCB_COPY_FROM_PARENT, window_, screen_->root, 0, 0, 10, 10, 0,
                      XCB_WINDOW_CLASS_INPUT_OUTPUT, screen_->root_visual, 0, nullptr);
    xcb_change_property(conn_, XCB_PROP_MODE_REPLACE, window_, atoms_[NET_WM_NAME], atoms_[UTF8_STRING], 8,
                        sizeof(kName) - 1, kName);
    xcb_change_property(conn_, XCB_PROP_MODE_REPLACE, screen_->root, atoms_[NET_SUPPORTING_WM_CHECK],
                        XCB_ATOM_WINDOW, 32, 1, &window_);
    xcb_change_property(conn_, XCB_PROP_MODE_REPLACE, window_, atoms_[NET_SUPPORTING_WM_CHECK], XCB_ATOM_WINDOW, 32,
                        1, &window_);
    xcb_set_selection_owner(conn_, window_, atoms_[WM_S0], XCB_CURRENT_TIME);
    xcb_set_selection_owner(conn_, window_, atoms_[NET_WM_CM_S0], XCB_CURRENT_TIME);

    no_focus_window_ = xcb_generate_id(conn_);
    const uint32_t values[] = {1, XCB_EVENT_MASK_KEY_PRESS | XCB_EVENT_MASK_KEY_RELEASE | XCB_EVENT_MASK_FOCUS_CHANGE};
    xcb_create_window(conn_, XCB_COPY_FROM_PARENT, no_focus_window_, screen_->root, -100, -100, 1, 1, 0,
                      XCB_WINDOW_CLASS_COPY_FROM_PARENT, XCB_COPY_FROM_PARENT,
                      XCB_CW_OVERRIDE_REDIRECT | XCB_CW_EVENT_MASK, values);
    xcb_map_window(conn_, no_focus_window_);
    xcb_flush(conn_);
}

Xwm::~Xwm() {
    if (!conn_)
        return;
    set_seat(nullptr, nullptr, nullptr);
    finish_selections();
    if (cursor_)
        xcb_free_cursor(conn_, cursor_);
    if (colormap_)
        xcb_free_colormap(conn_, colormap_);
    if (no_focus_window_)
        xcb_destroy_window(conn_, no_focus_window_);
    if (window_)
        xcb_destroy_window(conn_, window_);
    if (source_)
        wl_event_source_remove(source_);
    while (!surfaces_.empty())
        destroy_surface(surfaces_.front().get());
    xcb_disconnect(conn_);
}

void Xwm::set_cursor(const void* pixels, uint32_t stride, int w, int h, int hot_x, int hot_y) {
    if (!render_format_id_) {
        wlr_log(WLR_ERROR, "xwm: no 32-bit render format for the cursor");
        return;
    }
    if (cursor_)
        xcb_free_cursor(conn_, cursor_);
    const xcb_pixmap_t pix = xcb_generate_id(conn_);
    xcb_create_pixmap(conn_, 32, pix, screen_->root, uint16_t(w), uint16_t(h));
    const xcb_render_picture_t pic = xcb_generate_id(conn_);
    xcb_render_create_picture(conn_, pic, pix, render_format_id_, 0, nullptr);
    const xcb_gcontext_t gc = xcb_generate_id(conn_);
    xcb_create_gc(conn_, gc, pix, 0, nullptr);
    xcb_put_image(conn_, XCB_IMAGE_FORMAT_Z_PIXMAP, pix, gc, uint16_t(w), uint16_t(h), 0, 0, 0, 32,
                  stride * uint32_t(h), static_cast<const uint8_t*>(pixels));
    xcb_free_gc(conn_, gc);
    cursor_ = xcb_generate_id(conn_);
    xcb_render_create_cursor(conn_, cursor_, pic, uint16_t(hot_x), uint16_t(hot_y));
    xcb_free_pixmap(conn_, pix);
    xcb_render_free_picture(conn_, pic);
    xcb_change_window_attributes(conn_, screen_->root, XCB_CW_CURSOR, &cursor_);
    schedule_flush();
}

void Xwm::set_workareas(const std::vector<std::array<int, 4>>& areas) {
    std::vector<uint32_t> data;
    for (const auto& a : areas)
        for (int v : a)
            data.push_back(uint32_t(v));
    xcb_change_property(conn_, XCB_PROP_MODE_REPLACE, screen_->root, atoms_[NET_WORKAREA], XCB_ATOM_CARDINAL, 32,
                        uint32_t(data.size()), data.data());
    schedule_flush();
}

} // namespace atrium::xwayland
#endif
