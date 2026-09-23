#pragma once
#include "listener.hpp"
#include "placements.hpp"
#include "titlebar.hpp"

#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace atrium {

class Output;
class Server;
class Space;

// A top-level application window: an xdg_toplevel or an X11 window.
//
// Windows float. `geom` is the whole visible frame in layout coordinates: the
// title bar atrium draws (when the window has one) plus the client's content
// (xdg geometry, so client-side shadows are excluded). The view's scene tree
// sits at geom.x/geom.y. The position is ours; the content size is whatever the
// client last committed, requested through configure().
class View {
public:
    enum class Kind { Xdg, X11 };

    View(Server& server, Kind kind);
    virtual ~View();
    View(const View&) = delete;
    View& operator=(const View&) = delete;

    // --- backend specifics -------------------------------------------------
    virtual wlr_surface* surface() const = 0;
    virtual const char* app_id() const = 0;
    virtual const char* title() const = 0;
    virtual View* parent() const = 0;
    virtual void size_hints(wlr_box& min, wlr_box& max) const = 0;
    virtual bool is_dialog() const = 0;   // should be placed over its parent
    virtual bool modal() const { return false; }  // blocks its parent until closed
    // Where the window asked to be put (its content's top-left, in layout
    // coordinates); `user` when the user asked (X11 USPosition), not the app.
    virtual std::optional<std::pair<int, int>> requested_position(bool& user) const { (void)user; return std::nullopt; }
    virtual bool splash() const { return false; }   // a splash screen: centered, undecorated, no focus
    // Notifications, menus and tooltips that are ordinary windows: no
    // decoration, no focus, and not in the Dock, the switcher or the bar.
    virtual bool passive() const { return false; }
    virtual bool unmanaged() const { return false; }  // X11 override-redirect
    virtual bool wants_focus() const { return false; }
    virtual void close() = 0;
    // Layout position of the root surface's origin. For xdg windows that is
    // offset from geom by the client-side shadow margin.
    virtual void surface_origin(double& x, double& y) const { x = geom.x; y = geom.y + top(); }

    // Whether the client leaves decorating to atrium. Every window that allows
    // it gets atrium's title bar, so all windows look alike.
    virtual bool wants_ssd() const { return false; }
    // Add or drop the title bar after the client changed its mind.
    void refresh_decoration_mode();
    // Height of atrium's title bar above the content; 0 without one or fullscreen.
    int top() const;

    // --- window management -------------------------------------------------
    void move_to(int x, int y);
    // Ask for a new size and position. The size lands when the client commits.
    void request_geometry(wlr_box box);
    // In a secret space: large and centered, the blurred desktop showing
    // around it (fixed-size windows only center). leave_secret() gives back
    // the size it had before.
    void fit_secret(bool keep_box = true);
    void leave_secret();
    // Tiling: take `box` (remembering the floating one), or float again.
    void tile_to(const wlr_box& box);
    void untile();
    bool tiled() const { return tiled_; }
    void set_activated(bool activated);
    // `restore_geometry` false drops the maximized state where the window is
    // (resizing a maximized window) instead of returning to `restore`.
    void set_maximized(bool maximized, bool restore_geometry = true);
    void set_fullscreen(bool fullscreen);
    // Fill half or a quarter of the screen (geometry::snap_zone bits); TOP
    // alone maximizes.
    void snap(uint32_t zone);
    void unsnap(bool restore_geometry);
    // Apply windows.tiled_titlebars to a snapped window.
    void refresh_tiled_titlebar();
    void set_minimized(bool minimized);
    void raise();
    bool visible() const;

    // During an interactive resize from the left or top edge the opposite edge
    // stays put: when a new size arrives, x/y are derived from this anchor.
    void begin_resize(uint32_t edges);
    void end_resize();

    // Rounded corners, shadow and title bar from the current settings and focus state.
    void update_decorations();

    // Visual-only state for animations: opacity of the whole window and an
    // offset from its real position. Neither changes geometry or input.
    void set_alpha(float alpha);
    void set_anim_offset(int dx, int dy);
    float alpha() const { return alpha_; }

    std::unique_ptr<Titlebar> titlebar;

    Server& server;
    const Kind kind;
    const uint64_t id;  // stable for the view's lifetime; IPC addresses windows by it
    Output* output = nullptr;
    Space* space = nullptr;  // managed windows only
    wlr_scene_tree* tree = nullptr;      // root of the view, at geom.x/geom.y
    wlr_scene_tree* content = nullptr;   // the client's surfaces, at (0, top())
    wlr_scene_tree* popups = nullptr;    // xdg popups, at the content origin
    wlr_scene_shadow* shadow = nullptr;
    wlr_scene_rect* outline = nullptr;   // 1px hairline around the frame
    wlr_scene_blur* blur = nullptr;      // frosted glass behind translucent content
    wlr_scene_rect* backing = nullptr;   // solid fill behind the content with transparency off
    wlr_box geom{};
    wlr_box restore{};  // geometry to return to from maximized/fullscreen

    bool mapped = false;
    bool activated = false;
    bool minimized = false;
    bool maximized = false;
    bool fullscreen = false;
    uint32_t snapped = 0;  // snap zone the window fills, 0 when free
    bool urgent = false;
    bool keep_above = false;    // stays over other windows (X11 _NET_WM_STATE_ABOVE)
    bool keep_below = false;    // stays under them
    bool skip_taskbar = false;  // not in the Dock or the switcher
    bool sticky = false;        // follows you from space to space
    bool float_in_tiling = false;  // stays out of the tiles on a tiled space
    // Out of the Dock, the switcher and the bar's space icons.
    bool hidden_from_lists() const { return skip_taskbar || passive(); }
    std::string icon;  // an icon name, or the path of the picture the app sent
    std::string tag;   // the app's own name for this kind of window ("main", "prefs")

protected:
    // Backend hooks for the state changes above. `frame` includes the title
    // bar; content_box() is the part the client draws.
    virtual void configure(const wlr_box& frame) = 0;
    wlr_box content_box(const wlr_box& frame) const {
        return {frame.x, frame.y + top(), frame.width, frame.height - top()};
    }
    virtual void send_activated(bool activated) = 0;
    virtual void send_maximized(bool maximized) = 0;
    virtual void send_fullscreen(bool fullscreen) = 0;
    virtual void send_suspended(bool) {}
    virtual void notify_position() {}  // X11 windows are told where they are
    // True while the client has not yet acked the last size we asked for.
    virtual bool awaiting_configure() const { return false; }
    virtual wlr_scene_tree* create_content(wlr_scene_tree* parent) = 0;

    // Shared map/unmap/commit logic, called by the backends.
    void handle_map();
    void handle_unmap();
    // The client committed a new content size.
    void handle_size(int width, int height);
    void layout_frame();
    void update_title();
    // Cheap enough for every commit: subsurfaces come and go between resizes.
    void update_corners();

    wlr_box usable_area() const;

    // Only xdg windows need anchoring: an X11 configure carries the position
    // along with the size, so X11 windows are simply placed where asked.
    bool anchored() const { return resize_edges_ && kind == Kind::Xdg; }
    // The grab has ended but the final size has not landed yet; the anchor
    // holds until it does (see XdgView::commit).
    void settle_resize();

    void place_tree();  // tree at geom + animation offset

    // Hide the title bar for tiling; the frame shrinks by it, the content stays.
    void set_tile_bar_hidden(bool hidden);
    bool tile_bar_hidden_ = false;

    // Where the app's last window was, claimed for this one (see Server::placement_for).
    std::optional<Placement> remembered_;
    std::optional<wlr_box> before_secret_;  // its floating box before a secret space took it
    std::optional<wlr_box> before_tile_;    // ... before tiling took it
    bool tiled_ = false;

    uint32_t resize_edges_ = 0;
    bool resize_settling_ = false;
    float alpha_ = 1.0f;
    int anim_dx_ = 0, anim_dy_ = 0;
    int anchor_right_ = 0, anchor_bottom_ = 0;

private:
    void place();
    void animate_close();
    void set_output(Output* output);
    void create_toplevel_handles();
    void destroy_toplevel_handles();
    void update_output_from_position();

    wlr_ext_foreign_toplevel_handle_v1* ext_handle_ = nullptr;
    wlr_foreign_toplevel_handle_v1* handle_ = nullptr;
    wlr_scene* capture_scene_ = nullptr;
    wlr_ext_image_capture_source_v1* capture_source_ = nullptr;

    Listener<wlr_foreign_toplevel_handle_v1_activated_event> handle_activate_;
    Listener<wlr_foreign_toplevel_handle_v1_maximized_event> handle_maximize_;
    Listener<wlr_foreign_toplevel_handle_v1_minimized_event> handle_minimize_;
    Listener<wlr_foreign_toplevel_handle_v1_fullscreen_event> handle_fullscreen_;
    Listener<> handle_close_;

    friend class Server;  // image capture requests
};

// Wayland-native window.
class XdgView final : public View {
public:
    XdgView(Server& server, wlr_xdg_toplevel* toplevel);
    ~XdgView() override;

    wlr_surface* surface() const override { return toplevel->base->surface; }
    const char* app_id() const override;
    const char* title() const override;
    View* parent() const override;
    void size_hints(wlr_box& min, wlr_box& max) const override;
    bool is_dialog() const override;
    bool modal() const override;
    void close() override;
    void surface_origin(double& x, double& y) const override;

    void dismiss_popups();
    void set_decoration(wlr_xdg_toplevel_decoration_v1* decoration);

    wlr_xdg_toplevel* const toplevel;

    bool wants_ssd() const override;
    void set_kde_decoration(wlr_server_decoration* decoration);

protected:
    void configure(const wlr_box& frame) override;
    void send_activated(bool activated) override;
    void send_maximized(bool maximized) override;
    void send_fullscreen(bool fullscreen) override;
    void send_suspended(bool suspended) override;
    bool awaiting_configure() const override;
    wlr_scene_tree* create_content(wlr_scene_tree* parent) override;

private:
    void commit();

    uint32_t last_size_serial_ = 0;
    void apply_decoration_mode();

    wlr_xdg_toplevel_decoration_v1* decoration_ = nullptr;
    wlr_server_decoration* kde_decoration_ = nullptr;  // older KDE protocol (Qt5, GTK3 apps via it)
    wlr_box bounds_{};

    Listener<> commit_, map_, unmap_, destroy_;
    Listener<> request_fullscreen_, request_maximize_, request_minimize_;
    Listener<wlr_xdg_toplevel_move_event> request_move_;
    Listener<wlr_xdg_toplevel_resize_event> request_resize_;
    Listener<> set_title_, set_app_id_;
    Listener<> decoration_request_, decoration_destroy_;
    Listener<> kde_mode_, kde_destroy_;
};

// window_hints.cpp
void forget_icon(const View& view);  // delete the picture saved for it
const char* content_type_name(Server& server, const View& view);  // "none", "photo", "video", "game"
// The fullscreen window at the front of `output` when it asked to tear and
// display.allow_tearing lets it; otherwise null.
View* tearing_view(Server& server, const Output& output);

// Attach the popup machinery for a new xdg_popup (of a view or a layer surface).
void handle_new_xdg_popup(Server& server, wlr_xdg_popup* popup);

} // namespace atrium
