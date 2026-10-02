#pragma once
#include "wl/compositor.hpp"
#include "wl/positioner.hpp"

#include "xdg-shell-server.hpp"
#include "xx-pip-v1-server.hpp"

#include <optional>
#include <string>

namespace atrium::wl {

class Output;
class Popup;
class Seat;
class Shell;
class Toplevel;

// What a toplevel is told in a configure (and what it acks).
struct ToplevelState {
    int width = 0, height = 0;  // 0: the client picks
    bool maximized = false, fullscreen = false, resizing = false, activated = false, suspended = false;
    uint32_t tiled = 0, constrained = 0;  // edge bits: 1 top, 2 bottom, 4 left, 8 right
    bool operator==(const ToplevelState&) const = default;
};

// xdg_surface: a surface that is a window or a popup. It is the surface's
// role; the toplevel or popup object is what it becomes.
class Pip;

class ShellSurface : public XdgSurface, public Role {
public:
    static constexpr const char* kRole = "xdg_surface";
    enum class Kind { None, Toplevel, Popup, Pip };

    ShellSurface(wl_client* client, uint32_t version, uint32_t id, Shell& shell, Surface* surface);
    ~ShellSurface() override;

    // The xdg_surface a surface is, if any.
    static ShellSurface* from(Surface* surface);

    const char* name() const override { return kRole; }
    bool precommit(Surface& surface) override;
    void commit(Surface& surface) override;

    Surface* surface() const { return surface_; }
    Kind kind() const { return kind_; }
    Toplevel* toplevel() const { return toplevel_; }
    Popup* popup() const { return popup_; }
    Pip* pip() const { return pip_; }
    // The client has committed once, and is waiting for a first configure.
    bool initialized() const { return initialized_; }
    bool configured() const { return configured_; }
    // The window's visible part, in surface coordinates (its shadow and
    // other decorations outside it).
    Box geometry() const { return geometry_; }
    const std::vector<Popup*>& popups() const { return popups_; }

    // Sends a configure soon (batched): returns its serial.
    uint32_t schedule_configure();
    // The serial of the configure the current state answers.
    uint32_t configure_serial() const { return surface_ ? surface_->current().xdg_configure_serial : 0; }
    void ping();

    struct {
        Signal<uint32_t> configure;  // about to be sent, with this serial
        Signal<uint32_t> ack_configure;
        Signal<Popup*> new_popup;
        Signal<> destroy;
    } events;

private:
    friend class Toplevel;
    friend class Popup;
    friend class Pip;
    friend class Shell;
    friend class PipShell;
    void flush_configure();
    void reset();
    void update_geometry();
    void surface_gone();

    Shell& shell_;
    Surface* surface_;
    Kind kind_ = Kind::None;
    Toplevel* toplevel_ = nullptr;
    Popup* popup_ = nullptr;
    Pip* pip_ = nullptr;
    bool initialized_ = false, configured_ = false;
    Box geometry_;
    std::vector<Popup*> popups_;
    struct Sent {
        uint32_t serial;
        ToplevelState toplevel;
        Box popup;
        std::pair<int, int> pip;
    };
    std::vector<Sent> sent_;
    wl_event_source* idle_ = nullptr;
    uint32_t scheduled_serial_ = 0;
    Signal<>::Connection surface_gone_;
};

class Toplevel : public XdgToplevel {
public:
    Toplevel(wl_client* client, uint32_t version, uint32_t id, ShellSurface* base);
    ~Toplevel() override;

    static Toplevel* from(wl_resource* resource);
    static Toplevel* from(Surface* surface);

    ShellSurface* base() const { return base_; }
    const std::string& title() const { return title_; }
    const std::string& app_id() const { return app_id_; }
    Toplevel* parent() const { return parent_; }

    void* data = nullptr;  // the compositor's own object

    // What the client asked for; the compositor answers with a configure.
    struct Requested {
        bool maximized = false, fullscreen = false, minimized = false;
        Output* fullscreen_output = nullptr;
    };
    const Requested& requested() const { return requested_; }
    // The state being built for the next configure, the last one acked, and
    // the one in effect (acked, then committed).
    const ToplevelState& scheduled() const { return scheduled_; }
    const ToplevelState& current() const { return current_; }
    int min_width() const;
    int min_height() const;
    int max_width() const;
    int max_height() const;

    uint32_t set_size(int width, int height);
    uint32_t set_activated(bool on);
    uint32_t set_maximized(bool on);
    uint32_t set_fullscreen(bool on);
    uint32_t set_resizing(bool on);
    uint32_t set_tiled(uint32_t edges);
    uint32_t set_constrained(uint32_t edges);
    uint32_t set_suspended(bool on);
    uint32_t set_bounds(int width, int height);
    // WindowMenu 1, Maximize 2, Fullscreen 4, Minimize 8 (bits, atrium's own).
    uint32_t set_wm_capabilities(uint32_t caps);
    void close() { send_close(); }

    struct MoveRequest {
        Seat* seat;
        uint32_t serial;
    };
    struct ResizeRequest {
        Seat* seat;
        uint32_t serial;
        uint32_t edges;  // xdg_toplevel.resize_edge
    };
    struct MenuRequest {
        Seat* seat;
        uint32_t serial;
        int x, y;
    };
    struct {
        Signal<const MoveRequest&> request_move;
        Signal<const ResizeRequest&> request_resize;
        Signal<const MenuRequest&> request_window_menu;
        Signal<> request_maximize, request_fullscreen, request_minimize;
        Signal<> set_title, set_app_id, set_parent;
        Signal<> initial_commit;  // before the first configure goes out
        Signal<> destroy;
    } events;

private:
    friend class ShellSurface;
    void sent(ShellSurface::Sent& s);
    void acked(const ShellSurface::Sent& s);
    void committed();
    void reset();
    bool set_parent(Toplevel* parent);

    ShellSurface* base_;
    std::string title_, app_id_;
    Toplevel* parent_ = nullptr;
    Signal<>::Connection parent_unmap_, parent_destroy_;
    Requested requested_;
    ToplevelState scheduled_, acked_, current_;
    std::optional<std::pair<int, int>> bounds_;
    std::optional<uint32_t> wm_caps_;
    bool gone_ = false;
    void gone();
};

class Popup : public XdgPopup {
public:
    Popup(wl_client* client, uint32_t version, uint32_t id, ShellSurface* base, Surface* parent,
          const PositionerRules& rules);
    ~Popup() override;

    ShellSurface* base() const { return base_; }
    // The surface it hangs off: an xdg_surface's, or a layer surface's once
    // it adopts it (set_parent).
    Surface* parent() const { return parent_; }
    void set_parent(Surface* parent);
    const PositionerRules& rules() const { return rules_; }
    // Where it goes, relative to its parent's window geometry.
    Box geometry() const { return current_; }
    Box scheduled_geometry() const { return scheduled_; }

    void* data = nullptr;  // the compositor's own object

    // Keeps it inside `box` (in its parent's window-geometry coordinates).
    void unconstrain_from(const Box& box);
    // Dismissed: the client closes it.
    void dismiss();

    struct GrabRequest {
        Seat* seat;
        uint32_t serial;
    };
    struct {
        Signal<const GrabRequest&> request_grab;
        Signal<> reposition;  // new rules: unconstrain again
        Signal<> destroy;
    } events;

private:
    friend class ShellSurface;
    void gone();

    ShellSurface* base_;
    Surface* parent_ = nullptr;
    Signal<>::Connection parent_gone_;
    PositionerRules rules_;
    Box scheduled_, acked_, current_;
    std::optional<uint32_t> reposition_token_;
    bool gone_ = false;
    bool dismissed_ = false;
};

// xx_pip_v1: a picture-in-picture window (a video popped out of its
// page), kept above the others where the compositor likes.
class Pip : public XxPipV1 {
public:
    Pip(wl_client* client, uint32_t version, uint32_t id, ShellSurface* base);
    ~Pip() override;

    static Pip* from(Surface* surface);

    ShellSurface* base() const { return base_; }
    const std::string& app_id() const { return app_id_; }
    // Where it was launched from, for an animation (committed state).
    Surface* origin() const { return origin_; }
    const std::optional<Box>& origin_rect() const { return origin_rect_; }
    // The size in effect (acked, then committed); 0: the client's choice.
    std::pair<int, int> size() const { return current_; }

    uint32_t set_size(int width, int height);
    // The most it may grow to: sent with the next configure.
    uint32_t set_bounds(int width, int height);
    // It won't be shown again: the client should destroy it.
    void close() { send_closed(); }

    struct MoveRequest {
        Seat* seat;
        uint32_t serial;
    };
    struct ResizeRequest {
        Seat* seat;
        uint32_t serial;
        uint32_t edges;  // xx_pip_v1.resize_edge
    };
    struct {
        Signal<const MoveRequest&> request_move;
        Signal<const ResizeRequest&> request_resize;
        Signal<> set_app_id;
        Signal<> initial_commit;  // before the first configure goes out
        Signal<> destroy;
    } events;

private:
    friend class ShellSurface;
    void sent(ShellSurface::Sent& s);
    void committed();
    void reset();
    void gone();

    ShellSurface* base_;
    std::string app_id_;
    // Double-buffered with the surface's commit.
    Surface* pending_origin_ = nullptr;
    bool origin_pending_ = false;
    std::optional<Box> pending_origin_rect_;
    bool origin_rect_pending_ = false;
    Surface* origin_ = nullptr;
    std::optional<Box> origin_rect_;
    Signal<>::Connection pending_origin_gone_, origin_gone_;
    std::pair<int, int> scheduled_{}, acked_{}, current_{};
    std::optional<std::pair<int, int>> bounds_, sent_bounds_;
    bool gone_ = false;
};

// xx_pip_shell_v1: makes xdg_surfaces picture-in-picture windows.
class PipShell {
public:
    explicit PipShell(wl_display* display);
    ~PipShell();

    struct {
        Signal<Pip*> new_pip;
    } events;

private:
    std::unique_ptr<Global> global_;
    std::vector<Weak<Resource>> managers_;
};

class Positioner : public XdgPositioner {
public:
    Positioner(wl_client* client, uint32_t version, uint32_t id);
    PositionerRules rules;
};

// xdg_wm_base.
class Shell {
public:
    // `ping_timeout_ms`: how long a client may take to answer a ping.
    Shell(wl_display* display, int ping_timeout_ms = 10000);
    ~Shell();
    Shell(const Shell&) = delete;
    Shell& operator=(const Shell&) = delete;

    wl_display* display() const { return display_; }

    struct {
        Signal<ShellSurface*> new_surface;
        Signal<Toplevel*> new_toplevel;
        Signal<Popup*> new_popup;
        Signal<wl_client*> ping_timeout;
    } events;

private:
    friend class ShellSurface;
    friend class Popup;
    struct Client;
    Client* client_of(wl_client* client);
    void ping(wl_client* client);

    wl_display* display_;
    int ping_timeout_ms_;
    std::unique_ptr<Global> global_;
    std::vector<std::unique_ptr<Client>> clients_;
};

} // namespace atrium::wl
