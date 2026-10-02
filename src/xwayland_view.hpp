#pragma once
#ifdef ATRIUM_XWAYLAND
#include "scene/scene.hpp"
#include "view.hpp"
#include "xwayland/xwm.hpp"

namespace atrium {

// X11 window through Xwayland. Override-redirect windows (menus, tooltips,
// drag images) are "unmanaged": they place themselves, get no decorations and
// never enter the focus order.
class XwaylandView final : public View {
public:
    XwaylandView(Server& server, xwayland::XSurface* xsurface);
    ~XwaylandView() override;

    wl::Surface* surface() const override { return xsurface->surface; }
    const char* app_id() const override;
    const char* title() const override;
    View* parent() const override;
    void size_hints(wlr_box& min, wlr_box& max) const override;
    bool is_dialog() const override;
    bool modal() const override { return xsurface->modal && xsurface->parent; }
    bool unmanaged() const override { return xsurface->override_redirect; }
    bool wants_focus() const override;
    bool wants_ssd() const override;
    std::optional<std::pair<int, int>> requested_position(bool& user) const override;
    bool splash() const override;
    bool passive() const override;
    void close() override;

    xwayland::XSurface* const xsurface;

protected:
    void configure(const wlr_box& frame) override;
    void send_activated(bool activated) override;
    void send_maximized(bool maximized) override;
    void send_fullscreen(bool fullscreen) override;
    void notify_position() override { configure(geom); }
    scene::Tree* create_content(scene::Tree* parent) override;

private:
    bool has_type(xwayland::WindowType type) const { return xsurface->has_window_type(type); }
    // _NET_WM_STATE flags the window set (the XWM already updated them).
    void apply_states();

    void map();
    void unmap();
    void request_configure(const xwayland::XSurface::ConfigureRequest& event);
    void set_geometry();
    void commit();

    std::vector<wl::Connection> connections_;
    wl::Connection map_, unmap_, commit_;
};

} // namespace atrium
#endif
