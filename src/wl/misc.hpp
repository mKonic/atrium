#pragma once
#include "wl/xdg_shell.hpp"

#include <map>
#include <string>

namespace atrium::wl {

// zxdg_exporter_v2 / zxdg_importer_v2: one app's window becomes the parent
// of another app's dialog (a portal's file chooser over the app that asked).
class XdgForeign {
public:
    explicit XdgForeign(wl_display* display);
    ~XdgForeign();

    // A dialog is set as the child of an exported toplevel.
    Signal<Toplevel* /*child*/, Toplevel* /*parent*/> set_parent;

private:
    struct Exported;
    std::unique_ptr<Global> exporter_, importer_;
    std::vector<Weak<Resource>> managers_;
    std::vector<std::unique_ptr<Exported>> exported_;
};

// hyprland_global_shortcuts_manager_v1: apps (through the GlobalShortcuts
// portal) register shortcuts atrium fires when their keys are pressed.
class GlobalShortcuts {
public:
    struct Shortcut {
        std::string id, app_id, description, trigger_description;
        wl_client* client;
        Weak<Resource> resource;
    };

    explicit GlobalShortcuts(wl_display* display);
    ~GlobalShortcuts();

    const std::vector<std::unique_ptr<Shortcut>>& shortcuts() const { return shortcuts_; }
    void press(Shortcut* s, uint64_t time_ns);
    void release(Shortcut* s, uint64_t time_ns);

    Signal<Shortcut*> registered, unregistered;

private:
    std::unique_ptr<Global> global_;
    std::vector<Weak<Resource>> managers_;
    std::vector<std::unique_ptr<Shortcut>> shortcuts_;
};

// wp_security_context_manager_v1: a sandbox (Flatpak) opens a socket of its
// own for the app, tagged with who it is; atrium can then keep privileged
// globals (screen capture, virtual input) from it.
class SecurityContexts {
public:
    struct Metadata {
        std::string sandbox_engine, app_id, instance_id;
    };

    explicit SecurityContexts(wl_display* display);
    ~SecurityContexts();

    // Who a client is, if it came through a security context.
    const Metadata* lookup(const wl_client* client) const;

private:
    struct Listener;
    wl_display* display_;
    std::unique_ptr<Global> global_;
    std::vector<Weak<Resource>> managers_;
    std::vector<std::unique_ptr<Listener>> listeners_;
    std::map<const wl_client*, Metadata> clients_;
};

} // namespace atrium::wl
