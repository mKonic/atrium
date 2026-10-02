#pragma once
// atrium's X11 window manager for Xwayland, on its own protocol layer: a port
// of wlroots' xwayland/xwm.c and xwayland/selection (MIT). Xwayland itself is
// started by xwayland::Server.
#include "wl/compositor.hpp"
#include "wl/data_device.hpp"
#include "wl/signal.hpp"

#include <xcb/xcb.h>
#include <xcb/xcb_ewmh.h>
#include <xcb/xcb_icccm.h>
#include <xcb/xfixes.h>

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <vector>

struct wl_event_source;

namespace atrium::wl {
class PrimarySelection;
class XwaylandShell;
class XwaylandSurface;
} // namespace atrium::wl

namespace atrium::xwayland {

class Xwm;

enum class WindowType {
    Desktop,
    Dock,
    Toolbar,
    Menu,
    Utility,
    Splash,
    Dialog,
    DropdownMenu,
    PopupMenu,
    Tooltip,
    Notification,
    Combo,
    Dnd,
    Normal,
};

// ICCCM's input models: whether the window takes focus itself (WM_HINTS
// input) and whether it wants WM_TAKE_FOCUS.
enum class InputModel { None, Passive, Local, Global };

// What the window's Motif hints leave out.
enum Decorations : uint32_t { DecorationsAll = 0, DecorationsNoBorder = 1, DecorationsNoTitle = 2 };

// An X11 window. Its wl_surface (what is drawn) is only there between the
// associate and dissociate events.
class XSurface {
public:
    XSurface(Xwm& xwm, xcb_window_t window, int16_t x, int16_t y, uint16_t width, uint16_t height,
             bool override_redirect);
    ~XSurface();
    XSurface(const XSurface&) = delete;
    XSurface& operator=(const XSurface&) = delete;

    // The window drawn by `surface`, if any.
    static XSurface* from(wl::Surface* surface);

    Xwm& xwm;
    const xcb_window_t window_id;
    wl::Surface* surface = nullptr;
    uint32_t surface_id = 0;  // WL_SURFACE_ID still to pair
    uint64_t serial = 0;      // WL_SURFACE_SERIAL

    int16_t x, y;
    uint16_t width, height;
    bool override_redirect;
    float opacity = 1;

    std::optional<std::string> title, class_, instance, role, startup_id;
    pid_t pid = 0;

    XSurface* parent = nullptr;
    std::vector<XSurface*> children;

    std::vector<xcb_atom_t> window_type, protocols;
    uint32_t decorations = DecorationsAll;
    std::optional<xcb_icccm_wm_hints_t> hints;
    std::optional<xcb_size_hints_t> size_hints;
    std::optional<xcb_ewmh_wm_strut_partial_t> strut_partial;

    // _NET_WM_STATE
    bool modal = false, fullscreen = false, maximized_vert = false, maximized_horz = false, minimized = false,
         withdrawn = false, sticky = false, shaded = false, skip_taskbar = false, skip_pager = false,
         above = false, below = false, demands_attention = false;
    bool has_alpha = false;
    bool pinging = false;

    void* data = nullptr;  // the compositor's

    struct ConfigureRequest {
        int16_t x, y;
        uint16_t width, height;
        uint16_t mask;  // xcb_config_window_t
    };
    struct {
        wl::Signal<> destroy;
        wl::Signal<const ConfigureRequest&> request_configure;
        wl::Signal<> request_move;
        wl::Signal<uint32_t> request_resize;  // edges (Edges bits)
        wl::Signal<bool> request_minimize;
        wl::Signal<> request_maximize, request_fullscreen, request_activate, request_close;
        wl::Signal<> request_sticky, request_shaded, request_skip_taskbar, request_skip_pager;
        wl::Signal<> request_above, request_below, request_demands_attention;
        wl::Signal<> associate, dissociate;
        wl::Signal<> set_title, set_class, set_role, set_parent, set_startup_id, set_window_type, set_hints,
            set_size_hints, set_decorations, set_strut_partial, set_override_redirect, set_geometry, set_opacity,
            set_icon;
        wl::Signal<> focus_in, grab_focus, map_request, ping_timeout;
    } events;

    void activate(bool activated);
    // Restacks against `sibling` (null: all managed windows).
    void restack(XSurface* sibling, xcb_stack_mode_t mode);
    void configure(int16_t x, int16_t y, uint16_t width, uint16_t height);
    void close();
    void set_withdrawn(bool withdrawn);
    void set_minimized(bool minimized);
    void set_maximized(bool horz, bool vert);
    void set_fullscreen(bool fullscreen);
    void set_sticky(bool sticky);
    void set_shaded(bool shaded);
    void set_skip_taskbar(bool skip);
    void set_skip_pager(bool skip);
    void set_above(bool above);
    void set_below(bool below);
    void set_demands_attention(bool demands);
    void offer_focus();
    void ping();

    bool has_window_type(WindowType type) const;
    bool override_redirect_wants_focus() const;
    InputModel icccm_input_model() const;
    const char* title_or_empty() const { return title ? title->c_str() : ""; }

private:
    friend class Xwm;

    void set_net_wm_state();
    void set_wm_state();
    void dissociate();

    std::optional<std::string> wm_name_, net_wm_name_;
    wl_event_source* ping_timer_ = nullptr;
    wl::Connection commit_, map_, unmap_, surface_gone_;
};

// The X11 side of the clipboard, the primary selection or drag and drop.
struct Selection;

class Xwm {
public:
    // Takes over `wm_fd`. `client` is Xwayland's Wayland connection; without
    // `shell`, windows pair with surfaces by id only (older Xwayland).
    Xwm(wl_display* display, int wm_fd, wl_client* client, wl::Compositor& compositor, wl::XwaylandShell* shell,
        bool terminate_when_idle);
    ~Xwm();
    Xwm(const Xwm&) = delete;
    Xwm& operator=(const Xwm&) = delete;

    bool ok() const { return conn_ != nullptr; }
    xcb_connection_t* connection() const { return conn_; }
    xcb_screen_t* screen() const { return screen_; }
    wl_display* display() const { return display_; }

    // The seat's selections and drag and drop to bridge (null: none).
    void set_seat(wl::Seat* seat, wl::DataDevices* data, wl::PrimarySelection* primary);
    // The root window's cursor, ARGB8888 premultiplied.
    void set_cursor(const void* pixels, uint32_t stride, int width, int height, int hot_x, int hot_y);
    // _NET_WORKAREA: per screen, x y w h.
    void set_workareas(const std::vector<std::array<int, 4>>& areas);

    const std::vector<std::unique_ptr<XSurface>>& surfaces() const { return surfaces_; }
    XSurface* focused() const { return focus_; }

    struct RemoveStartupInfo {
        const char* id;
        xcb_window_t window;
    };
    struct {
        wl::Signal<XSurface*> new_surface;
        wl::Signal<const RemoveStartupInfo&> remove_startup_info;
        // The X server went (it hung up): the compositor deletes this.
        wl::Signal<> hangup;
    } events;

    // Atoms (xwayland/atoms.hpp).
    xcb_atom_t atom(int name) const { return atoms_[name]; }
    std::string atom_name(xcb_atom_t atom);

private:
    friend class XSurface;
    friend struct Selection;
    friend struct Transfer;
    friend class XDataSource;

    XSurface* lookup(xcb_window_t window) const;
    void schedule_flush();
    void send_wm_message(XSurface* s, const xcb_client_message_data_t& data, uint32_t event_mask);
    void set_net_active_window(xcb_window_t window);
    void set_net_client_list();
    void set_net_client_list_stacking();
    void focus_window(XSurface* s);
    void set_focused_window(XSurface* s);
    void surface_activate(XSurface* s);
    void update_override_redirect(XSurface* s, bool override_redirect);
    void associate(XSurface* s, wl::Surface* surface);
    void destroy_surface(XSurface* s);
    void read_property(XSurface* s, xcb_atom_t property, xcb_get_property_reply_t* reply);
    bool atoms_contain(const std::vector<xcb_atom_t>& atoms, int name) const;

    int on_event(uint32_t mask);
    int read_events();
    void handle_create(xcb_create_notify_event_t* ev);
    void handle_destroy(xcb_destroy_notify_event_t* ev);
    void handle_configure_request(xcb_configure_request_event_t* ev);
    void handle_configure_notify(xcb_configure_notify_event_t* ev);
    void handle_map_request(xcb_map_request_event_t* ev);
    void handle_map_notify(xcb_map_notify_event_t* ev);
    void handle_unmap_notify(xcb_unmap_notify_event_t* ev);
    void handle_property_notify(xcb_property_notify_event_t* ev);
    void handle_client_message(xcb_client_message_event_t* ev);
    void handle_focus_in(xcb_focus_in_event_t* ev);
    void handle_net_wm_state(xcb_client_message_event_t* ev);
    void handle_startup_info(xcb_client_message_event_t* ev);

    // Selections (selection.cpp).
    void init_selections();
    void finish_selections();
    bool selection_window(xcb_window_t window) const;
    bool handle_selection_event(xcb_generic_event_t* ev);
    bool handle_selection_client_message(xcb_client_message_event_t* ev);
    void handle_selection_destroy_notify(xcb_destroy_notify_event_t* ev);
    Selection* selection_for(xcb_atom_t atom);
    xcb_atom_t mime_to_atom(const std::string& mime);
    std::optional<std::string> mime_from_atom(xcb_atom_t atom);
    void start_drag(wl::Drag* drag);
    void set_drag_focus(XSurface* focus);
    void dnd_send(xcb_atom_t type, const xcb_client_message_data_t& data);

    wl_display* display_;
    wl_client* client_;
    wl::XwaylandShell* shell_;
    xcb_connection_t* conn_ = nullptr;
    xcb_screen_t* screen_ = nullptr;
    wl_event_source* source_ = nullptr;
    std::vector<xcb_atom_t> atoms_;
    xcb_window_t window_ = 0, no_focus_window_ = 0;
    xcb_visualid_t visual_id_ = 0;
    xcb_colormap_t colormap_ = 0;
    uint32_t render_format_id_ = 0;
    uint32_t cursor_ = 0;
    const xcb_query_extension_reply_t* xfixes_ = nullptr;
    const xcb_query_extension_reply_t* xres_ = nullptr;
    uint32_t xfixes_major_ = 0;
    uint16_t last_focus_seq_ = 0;
    uint32_t ping_timeout_ms_ = 10000;

    std::vector<std::unique_ptr<XSurface>> surfaces_;  // creation order
    std::vector<XSurface*> stack_;                       // bottom to top, managed and mapped
    std::vector<XSurface*> unpaired_;
    XSurface* focus_ = nullptr;
    XSurface* offered_focus_ = nullptr;
    struct PendingStartup {
        xcb_window_t window;
        std::string msg;
    };
    std::vector<PendingStartup> pending_startup_;

    wl::Seat* seat_ = nullptr;
    wl::DataDevices* data_ = nullptr;
    wl::PrimarySelection* primary_ = nullptr;
    std::unique_ptr<Selection> clipboard_, primary_selection_, dnd_;
    wl::Drag* drag_ = nullptr;
    XSurface* drag_focus_ = nullptr;
    XSurface* drop_focus_ = nullptr;
    bool drag_accepted_ = false;
    wl::DataSource* drop_source_ = nullptr;  // a dropped drag's, until XdndFinished
    wl::Connection new_surface_, shell_surface_, clipboard_changed_, primary_changed_, drag_started_;
    wl::Connection drag_focus_c_, drag_moved_, drag_dropped_, drag_ended_, drop_source_gone_;
};

} // namespace atrium::xwayland
