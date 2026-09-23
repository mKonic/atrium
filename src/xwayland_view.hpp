#pragma once
#ifdef ATRIUM_XWAYLAND
#include "view.hpp"

namespace atrium {

// X11 window through Xwayland. Override-redirect windows (menus, tooltips,
// drag images) are "unmanaged": they place themselves, get no decorations and
// never enter the focus order.
class XwaylandView final : public View {
public:
    XwaylandView(Server& server, wlr_xwayland_surface* xsurface);
    ~XwaylandView() override;

    wlr_surface* surface() const override { return xsurface->surface; }
    const char* app_id() const override;
    const char* title() const override;
    View* parent() const override;
    void size_hints(wlr_box& min, wlr_box& max) const override;
    bool is_dialog() const override;
    bool modal() const override { return xsurface->modal && xsurface->parent; }
    bool unmanaged() const override { return xsurface->override_redirect; }
    bool wants_focus() const override;
    bool wants_ssd() const override;
    void close() override;

    wlr_xwayland_surface* const xsurface;

protected:
    void configure(const wlr_box& frame) override;
    void send_activated(bool activated) override;
    void send_maximized(bool maximized) override;
    void send_fullscreen(bool fullscreen) override;
    void notify_position() override { configure(geom); }
    wlr_scene_tree* create_content(wlr_scene_tree* parent) override;

private:
    void map();
    void unmap();
    void request_configure(wlr_xwayland_surface_configure_event* event);
    void set_geometry();
    void commit();

    Listener<> associate_, dissociate_, destroy_;
    Listener<> map_, unmap_, commit_;
    Listener<> request_activate_, request_fullscreen_, request_maximize_, request_close_;
    Listener<wlr_xwayland_minimize_event> request_minimize_;
    Listener<wlr_xwayland_surface_configure_event> request_configure_;
    Listener<> request_move_;
    Listener<wlr_xwayland_resize_event> request_resize_;
    Listener<> set_geometry_, set_hints_, set_title_, set_class_, set_decorations_;
};

} // namespace atrium
#endif
