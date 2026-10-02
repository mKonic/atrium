#pragma once
#include "wl/compositor.hpp"

#include "xwayland-shell-v1-server.hpp"

namespace atrium::wl {

class XwaylandShell;

// xwayland_surface_v1: pairs a wl_surface with an X11 window, through the
// serial Xwayland also puts on the window (WL_SURFACE_SERIAL).
class XwaylandSurface : public XwaylandSurfaceV1, public Role {
public:
    static constexpr const char* kRole = "xwayland_surface_v1";

    XwaylandSurface(wl_client* client, uint32_t version, uint32_t id, Surface* surface, XwaylandShell& shell);
    ~XwaylandSurface() override;

    const char* name() const override { return kRole; }
    void commit(Surface& surface) override;

    Surface* surface() const { return surface_; }
    uint64_t serial() const { return serial_; }

private:
    XwaylandShell& shell_;
    Surface* surface_;
    uint64_t serial_ = 0;
    bool announced_ = false;
    Connection surface_gone_;
};

// xwayland_shell_v1: only for the Xwayland server's own client.
class XwaylandShell {
public:
    explicit XwaylandShell(wl_display* display);
    ~XwaylandShell();
    XwaylandShell(const XwaylandShell&) = delete;
    XwaylandShell& operator=(const XwaylandShell&) = delete;

    // The one client that may bind it (null: none).
    void set_client(wl_client* client) { client_ = client; }
    wl_client* client() const { return client_; }
    wl_global* global() const { return global_->global(); }

    // The surface whose committed serial is `serial`, if any.
    Surface* surface_from_serial(uint64_t serial) const;

    Signal<XwaylandSurface*> new_surface;  // a serial was committed

private:
    wl_client* client_ = nullptr;
    std::unique_ptr<Global> global_;
    std::vector<Weak<XwaylandSurface>> surfaces_;
};

} // namespace atrium::wl
