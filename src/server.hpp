#pragma once
#include "anim.hpp"
#include "config.hpp"
#include "keyword_watch.hpp"
#include "listener.hpp"
#include "scene/scene.hpp"
#include "protocols.hpp"

#include <nlohmann/json_fwd.hpp>

#include <array>
#include <filesystem>

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace atrium::xwayland {
class Xwm;
class XSurface;
} // namespace atrium::xwayland

namespace atrium {

struct Interface;  // theme.hpp

class Ipc;
class LayerSurface;
class Output;
class Titlebar;
class Seat;
class InputMethodRelay;
class BackgroundEffects;
class SystemBell;
class GlassShapes;
class ToplevelDrags;
class SessionManagement;
class ToplevelIcons;
class NightLight;
class SessionLock;
class Overview;
class Registry;
struct Placement;
class SnapPreview;
class Switcher;
class ShellProcess;
class Space;
class Settings;
class View;

// Scene layers, bottom to top.
enum class Layer : int {
    Background,
    Bottom,
    Views,
    Top,
    Fullscreen,
    Secret,      // a secret space over everything, on its dimmed backdrop
    Overview,    // Mission Control
    Unmanaged,   // X11 override-redirect: menus, tooltips
    Overlay,
    InputPopup,
    Lock,
    Count,
};
constexpr int kLayerCount = int(Layer::Count);

// What is under a point in layout coordinates.
struct Hit {
    wl::Surface* surface = nullptr;
    View* view = nullptr;
    LayerSurface* layer = nullptr;
    Titlebar* titlebar = nullptr;  // atrium's own title bar (then surface is null)
    Space* backdrop = nullptr;     // the dimmed screen behind a shown secret space
    double sx = 0, sy = 0;         // surface- or title-bar-local
};

// Which atrium object a surface belongs to. Popups resolve to the
// toplevel or layer surface they hang off.
struct Owner {
    View* view = nullptr;
    LayerSurface* layer = nullptr;
    explicit operator bool() const { return view || layer; }
};

class Server {
public:
    // An app's global shortcut (Action::Portal) pressed or let go.
    void portal_shortcut(const std::string& arg, bool pressed);
    // The layout changed (a switch key, a new keymap, a window's own): tell
    // the shell, and remember it for the window with keyboard.per_window.
    void keyboard_layout_changed();
    Server(Config defaults, bool nested, std::filesystem::path settings_file);
    ~Server();
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    void run(const char* startup_cmd);
    void quit();

    scene::Tree* layer(Layer l) const { return layers_[int(l)]; }

    Output* output_at(double lx, double ly) const;
    // `through`: a window the pointer looks through (one riding a drag).
    Hit hit_test(double lx, double ly, View* through = nullptr) const;
    Hit hit_test_scene(double lx, double ly) const;
    static Owner owner_of(wl::Surface* surface);

    // Focus. `raise` also brings the view to the top of the stack.
    void focus_view(View* view, bool raise = true);
    void focus_layer(LayerSurface* layer);
    void focus_top();
    // The focused window stops being focused (and IPC subscribers hear so).
    void drop_focus();
    // Fullscreen windows go over the panels only while in front: one with
    // another window brought over it (alt-tab, an app launched) drops back
    // among the windows, under that one.
    void restack_fullscreen();
    // Where the Dock shows each app's icon (Dock surface pixels), with the
    // windows it stands for: minimising shrinks a window into its own icon.
    struct DockIcon {
        wlr_box box;
        std::vector<uint64_t> windows;
    };
    std::map<std::string, std::vector<DockIcon>> dock_icons;  // by output name
    // The layout box of the Dock icon a window minimises into, if shown.
    std::optional<wlr_box> dock_icon_of(const View& view) const;
    View* top_view(Output* output) const;
    void cycle_focus(int direction);

    // --- spaces --------------------------------------------------------------
    Space* find_space(Output* output, int number) const;
    Space* ensure_space(Output* output, int number);
    Space* find_secret(const std::string& name) const;
    Space* ensure_secret(const std::string& name);
    // `carry` comes along and holds still while the spaces slide.
    void switch_space(Output* output, int number, View* carry = nullptr);
    void step_space(int direction);
    // Next or previous existing space on the focused output, wrapping around.
    void cycle_space(int direction);
    void move_to_space(View* view, Space* space);
    // A window command: geometry::named_place, or maximize, restore,
    // next-display / prev-display (the same place on the next screen).
    void place_window(View* view, const std::string& name);
    // The space before (-1) or after (+1) on the view's screen, taking the
    // view along (a window dragged against the screen's end). False if none.
    bool carry_to_space(View* view, int direction);
    // windows.fullscreen_space: into a space of its own on fullscreen, and
    // back home after.
    void fullscreen_space(View* view);
    // Tiling (tiling.cpp).
    void toggle_tiling(Space* space);
    void retile(Space* space);         // lay a tiled space out again; nothing if it floats
    bool tileable(const View* view) const;
    // A tiled window dropped at a point: it trades places with the one there.
    void tile_drop(View* view, double lx, double ly);
    // Keyboard navigation (tiling.cpp): the window beside `from` on screen,
    // and moving one that way (tiled: trade places; floating: snap).
    static uint32_t direction_from(const std::string& word);  // "left" → WLR_EDGE_LEFT
    View* neighbor_of(View* from, uint32_t direction) const;
    void move_direction(View* view, uint32_t direction);
    void toggle_secret(const std::string& name);
    void hide_secret();
    // Make `space` visible: switch to it, or show it if it is secret.
    void reveal(Space* space);
    // Delete a numbered space nobody is looking at and nothing lives in.
    void prune_space(Space* space);
    void spaces_changed();
    void fade_secret(Space* space, bool in);
    // Where a new window should open, when it is its app's first window and
    // the app has closed one before.
    std::optional<Placement> placement_for(const View* view) const;
    void remember_placement(const View* view);
    // The shell's menu for `view` (minimize, zoom, spaces, close...) at a
    // point in the layout.
    void show_window_menu(const View* view, double lx, double ly);
    // Where a window is, relative to its output, as it would be remembered.
    Placement placement_of(const View* view) const;

    // Decide a new window's space from the rules; returns what else they ask for.
    RuleResult assign_space(View* view);
    void output_added(Output* output);
    void output_removing(Output* output);

    void update_outputs();

    // Monitors are remembered by make, model and serial; plugged in again,
    // one comes back as it was set up.
    std::string display_id(const wlr_output* output) const;
    void remember_displays();
    void restore_display(Output* output);
    // An output change from IPC: { output, width, height, refresh, scale,
    // transform, x, y, enabled }. The error when it didn't take.
    std::optional<std::string> configure_output(const nlohmann::json& request);
    // A virtual screen: another window when running nested, else a headless
    // output (for streaming or a remote desktop). Returns its name.
    std::optional<std::string> create_output();
    // Only virtual screens can be taken away; an error otherwise.
    std::optional<std::string> remove_output(const std::string& name);
    void check_idle_inhibitors();
    void spawn(const std::string& command);
    void change_vt(unsigned vt);
    void run_action(const Keybind& bind);

    // A setting changed through the store: refresh `config`, persist, apply
    // the side effects and tell subscribers.
    void setting_changed(const std::string& key);
    // The registry's apps, rules or shortcuts changed: rebuild what they drive.
    void rebuild_from_registry();

    // Tell IPC subscribers about a window event ("opened", "closed",
    // "changed", "focused").
    void notify_window(const View& view, const char* what);

    Config config;
    // Snippet keywords being typed (launcher.snippet_expansion).
    KeywordWatch keywords;
    const bool nested;
    Animator animator{*this};

    wl_display* display = nullptr;
    wl_event_loop* loop = nullptr;
    wlr_backend* backend = nullptr;
    wlr_session* session = nullptr;
    wlr_renderer* renderer = nullptr;
    wlr_allocator* allocator = nullptr;
    // Every Wayland global (protocols.hpp).
    std::unique_ptr<Protocols> wl;
    scene::Scene* scene = nullptr;
    scene::Tree* drag_icons = nullptr;
    scene::Rect* root_bg = nullptr;
    scene::Rect* locked_bg = nullptr;
    // A blurred copy of everything below the windows (wallpaper, bottom
    // panels), rendered once per frame and sampled by every window's blur.
    scene::BlurCache* background_blur = nullptr;
    void apply_blur_settings();
    void apply_screen_shader();

    wlr_output_layout* output_layout = nullptr;
    wlr_box layout_box{};
#ifdef ATRIUM_XWAYLAND
    // Xwayland: the X server (its process, wlroots'), and atrium's own
    // window manager for it, made once it is ready.
    wlr_xwayland_server* xwayland = nullptr;
    std::unique_ptr<xwayland::Xwm> xwm;
#endif

    std::unique_ptr<Settings> settings;
    std::unique_ptr<SnapPreview> snap_preview;
    std::unique_ptr<Overview> overview;
    std::unique_ptr<Switcher> switcher;
    std::unique_ptr<Registry> registry;
    std::unique_ptr<ShellProcess> shell;
    std::unique_ptr<Ipc> ipc;
    std::unique_ptr<Seat> seat;
    std::unique_ptr<InputMethodRelay> input_method;
    std::unique_ptr<BackgroundEffects> background_effects;
    std::unique_ptr<SystemBell> system_bell;
    std::unique_ptr<GlassShapes> glass_shapes;
    std::unique_ptr<ToplevelDrags> toplevel_drags;
    std::unique_ptr<SessionManagement> sessions;
    std::unique_ptr<ToplevelIcons> toplevel_icons;
    std::unique_ptr<NightLight> night_light;
    uint64_t next_view_id = 1;
    std::vector<Output*> outputs;
    Output* focused_output = nullptr;

    // Managed, mapped views in focus order: most recently focused first.
    std::vector<View*> views;
    View* focused_view = nullptr;

    SessionLock* lock = nullptr;
    bool locked = false;

    std::vector<std::unique_ptr<Space>> spaces;
    Space* shown_secret = nullptr;

    // Set during teardown: objects dying with the backend skip relayout.
    bool shutting_down = false;

private:
    void setup();
    void teardown();
    void prepare_session_environment();
    void start_clipboard_history();
    void start_clipboard_sync();
    void restore_power_and_brightness();
    void apply_power_profile();
    Interface interface() const;
#ifdef ATRIUM_XWAYLAND
    void allow_root_x11(const char* display);
#endif
    void new_output(wlr_output* wlr);
    void setup_protocols();
    // Output management, power and gamma's protocol side (displays.cpp).
    void setup_outputs_protocols();
    // One screen's part of a layout to test or apply.
    struct OutputChange {
        Output* output;
        bool enabled;
        wlr_output_mode* mode = nullptr;      // a mode it has, or...
        std::optional<std::array<int, 3>> custom;  // ...width, height, refresh (mHz)
        float scale;
        wl_output_transform transform;
        int x, y;
        std::optional<bool> adaptive_sync;
    };
    // How the screens are now, as a change that changes nothing.
    std::vector<OutputChange> current_outputs() const;
    bool commit_output_config(const std::vector<OutputChange>& changes, bool test);
    // Tells output-management clients how the screens are.
    void publish_outputs();
    // Screen and window capture (capture.cpp).
    void setup_capture();
    struct CaptureState;
    std::unique_ptr<CaptureState> capture_;
    void capture_output_frame(Output* output, wlr_output_event_commit* event);
    void capture_view_frame(View* view, wlr_buffer* frame, const timespec& when);
public:
    // A window's capture scene, or a screen, is going: its captures stop.
    void capture_view_gone(View* view);
    void capture_output_gone(Output* output);
private:
    void gpu_reset();
    void disconnect_listeners();
    void workspace_requests(const std::vector<wl::Workspaces::Request>& requests);
    // Hints windows give about themselves: icon, tag, modal dialogs,
    // content type and tearing (window_hints.cpp).
    void setup_window_hints();

    scene::Tree* layers_[kLayerCount]{};
    wlr_backend* headless_ = nullptr;  // made on the first create_output() without a nested backend
    wl_display* layout_display_ = nullptr;  // the output layout's own (see setup)
    void seed_registry(const std::filesystem::path& dir);

    pid_t startup_pid_ = -1;
    std::string startup_cmd_;               // until Xwayland is up
    bool session_target_ = false;  // atrium-session.target may be ours to stop
    std::filesystem::path session_marker_;  // "running": ours while we run
    void note_last_session();
    wl_event_source* startup_timer_ = nullptr;
    void run_startup();

    Listener<wlr_output> new_output_;
    Listener<> layout_change_;
    Listener<> gpu_reset_;
    Listener<> backend_destroy_;
    // On the protocols' signals.
    std::vector<wl::Connection> connections_;
#ifdef ATRIUM_XWAYLAND
    Listener<> xwayland_start_;
    Listener<wlr_xwayland_server_ready_event> xwayland_ready_;
    wl::Connection new_x11_window_, xwm_hangup_;
#endif
};

} // namespace atrium
