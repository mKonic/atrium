#pragma once
#include "wl/xdg_shell.hpp"

#include <chrono>
#include <map>

namespace atrium::wl {

// zxdg_decoration_manager_v1 and KDE's org_kde_kwin_server_decoration:
// who draws a window's title bar. A client says what it would like; the
// compositor decides and tells it.
class Decorations {
public:
    enum Mode : uint32_t { Unset = 0, ClientSide = 1, ServerSide = 2 };

    // `kde_default`: what KDE's protocol announces before a client asks.
    Decorations(wl_display* display, Mode kde_default = ServerSide);
    ~Decorations();
    Decorations(const Decorations&) = delete;
    Decorations& operator=(const Decorations&) = delete;

    // One toplevel's xdg decoration object.
    struct Xdg {
        Toplevel* toplevel;
        Mode requested = Unset;  // what the client asked for (Unset: no preference)
        Mode scheduled = Unset;  // what the compositor decided, for the next configure
        Weak<Resource> resource;
        Signal<>::Connection configure, toplevel_gone;
    };
    // A KDE decoration: the surface and its mode.
    struct Kde {
        Surface* surface;
        Mode mode;
        Weak<Resource> resource;
        Signal<>::Connection surface_gone;
    };

    Xdg* xdg_for(Toplevel* toplevel);
    Kde* kde_for(Surface* surface);
    // Decides: sent with the toplevel's next configure.
    void set_mode(Xdg* decoration, Mode mode);

    struct {
        Signal<Xdg*> new_xdg;
        Signal<Xdg*> request_mode;  // the client changed its mind
        Signal<Xdg*> destroy_xdg;
        Signal<Kde*> new_kde, kde_mode, destroy_kde;
    } events;

private:
    Mode kde_default_;
    std::unique_ptr<Global> xdg_global_, kde_global_;
    std::vector<Weak<Resource>> managers_;
    std::vector<std::unique_ptr<Xdg>> xdg_;
    std::vector<std::unique_ptr<Kde>> kde_;
};

// xdg_wm_dialog_v1: a toplevel says it is a (modal) dialog of its parent.
class Dialogs {
public:
    explicit Dialogs(wl_display* display);
    ~Dialogs();

    // Whether `toplevel` asked to be modal.
    bool modal(Toplevel* toplevel) const;
    Signal<Toplevel*> changed;

private:
    struct Dialog {
        Toplevel* toplevel;
        bool modal = false;
        Weak<Resource> resource;
        Signal<>::Connection toplevel_gone;
    };
    std::unique_ptr<Global> global_;
    std::vector<Weak<Resource>> managers_;
    std::vector<std::unique_ptr<Dialog>> dialogs_;
};

// xdg_activation_v1: an app hands focus to another (a link opened in the
// browser, a notification's app), with a token made from real input.
class Activation {
public:
    Activation(wl_display* display, Seat& seat, std::chrono::milliseconds token_lifetime = std::chrono::seconds(30));
    ~Activation();

    struct Token {
        std::string name;
        Seat* seat = nullptr;  // set when it came from real input on this seat
        uint32_t serial = 0;
        std::string app_id;
        Surface* surface = nullptr;  // who asked, if said
        std::chrono::steady_clock::time_point made;
        Signal<>::Connection surface_gone;
    };
    // atrium's own, for an app it starts (XDG_ACTIVATION_TOKEN).
    std::string make_token(const std::string& app_id);

    struct Request {
        Surface* surface;
        const Token* token;  // null: unknown or expired
    };
    Signal<const Request&> request_activate;

private:
    Token* find(const std::string& name);
    void expire();

    Seat& seat_;
    std::chrono::milliseconds lifetime_;
    std::unique_ptr<Global> global_;
    std::vector<Weak<Resource>> managers_, tokens_;
    std::vector<std::unique_ptr<Token>> made_;
};

// xdg_toplevel_tag_manager_v1: a stable name for each of an app's windows
// (so rules can tell them apart), and a description of it.
class ToplevelTags {
public:
    explicit ToplevelTags(wl_display* display);
    ~ToplevelTags();

    struct Event {
        Toplevel* toplevel;
        std::string tag, description;
    };
    Signal<const Event&> set_tag, set_description;

private:
    std::unique_ptr<Global> global_;
    std::vector<Weak<Resource>> managers_;
};

} // namespace atrium::wl
