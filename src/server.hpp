#pragma once
#include "anim.hpp"
#include "backend/allocator.hpp"
#include "backend/headless.hpp"
#include "backend/session.hpp"
#include "input/libinput.hpp"
#include "cursor.hpp"
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

namespace atrium::backend::drm {
class Drm;
}

namespace atrium::xwayland {
class Server;
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
class LockScreen;
class Logind;
class Idle;
class Logout;
class ClipboardHistory;
class ScreenCast;
class Eis;
class Xsmp;
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
        Box box;
        std::vector<uint64_t> windows;
    };
    std::map<std::string, std::vector<DockIcon>> dock_icons;  // by output name
    // The layout box of the Dock icon a window minimises into, if shown.
    std::optional<Box> dock_icon_of(const View& view) const;
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
    static uint32_t direction_from(const std::string& word);  // "left" → EDGE_LEFT
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
    std::string display_id(const backend::Output* output) const;
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
    // Input happened (idle clients and atrium's own idle actions start over).
    void note_activity();
    // A notification from atrium itself (through the shell's server).
    void notify(const std::string& summary, const std::string& body);
    // The power button was pressed: false leaves it to logind.
    bool power_button();
    // A screen on or off (DPMS): idle, and clients through wlr-output-power.
    void set_screen_power(Output* output, bool on);
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
    // Where screens come from: the GPUs, windows in the host session when
    // nested, or memory (headless).
    backend::Multi* backend = nullptr;
    // Where libinput opens devices: atrium's session.
    std::unique_ptr<input::Libinput::DeviceSeat> device_seat;
    render::Renderer* renderer = nullptr;
    backend::Allocator* allocator = nullptr;
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

    OutputLayout* output_layout = nullptr;
    // The pointer's place and image.
    std::unique_ptr<Cursor> cursor;
    Box layout_box{};
#ifdef ATRIUM_XWAYLAND
    // Xwayland: the X server (its process and sockets), and atrium's own
    // window manager for it, made once it is ready.
    std::unique_ptr<xwayland::Server> xwayland;
    std::unique_ptr<xwayland::Xwm> xwm;
#endif

    std::unique_ptr<Settings> settings;
    std::unique_ptr<SnapPreview> snap_preview;
    std::unique_ptr<Overview> overview;
    std::unique_ptr<Switcher> switcher;
    std::unique_ptr<Registry> registry;
    std::unique_ptr<ShellProcess> shell;
    std::unique_ptr<LockScreen> lock_screen;
    std::unique_ptr<Logind> logind;  // after lock_screen: it holds on to it
    std::unique_ptr<Idle> idle;
    std::unique_ptr<Logout> logout;  // apps asked to quit, the session about to end
    std::unique_ptr<ClipboardHistory> clipboard_history;
    std::unique_ptr<ScreenCast> screencast;  // PipeWire streams for screen sharing
    std::unique_ptr<Eis> eis;                // remote input (libei) for RemoteDesktop sessions
    std::unique_ptr<Xsmp> xsmp;              // the X11 session manager X11 apps save through
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
    void start_phone_link();
    void restore_power_and_brightness();
    void apply_power_profile();
    Interface interface() const;
#ifdef ATRIUM_XWAYLAND
    void allow_root_x11(const char* display);
#endif
    void new_output(backend::Output* wlr);
    void setup_protocols();
    // Output management, power and gamma's protocol side (displays.cpp).
    void setup_outputs_protocols();
    // One screen's part of a layout to test or apply.
    struct OutputChange {
        Output* output;
        bool enabled;
        const backend::Mode* mode = nullptr;  // a mode it has, or...
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
    void capture_output_frame(Output* output, const backend::OutputState& state);
    void capture_view_frame(View* view, Buffer* frame, const timespec& when);
public:
    // A window's capture scene, or a screen, is going: its captures stop.
    void capture_view_gone(View* view);
    void capture_output_gone(Output* output);
    // A frame of `output` is about to be drawn / its pointer drawn into it:
    // copies that want no pointer get what's under it.
    void capture_before_frame(Output* output);
    void capture_before_cursor(const backend::Output* screen, render::RenderPass* pass);
    // Its copies without the pointer still have it (drawn in software
    // through the blend buffer).
    bool capture_keeps_pointer(Output* output) const;
private:
    void gpu_reset();
    void disconnect_listeners();
    void workspace_requests(const std::vector<wl::Workspaces::Request>& requests);
    // Hints windows give about themselves: icon, tag, modal dialogs,
    // content type and tearing (window_hints.cpp).
    void setup_window_hints();

    scene::Tree* layers_[kLayerCount]{};
    std::unique_ptr<backend::Session> own_session_;
    backend::drm::Drm* primary_gpu_ = nullptr;
    std::vector<backend::drm::Drm*> pending_gpu_removal_;
    wl::Connection gpu_added_;
    std::vector<wl::Connection> gpu_removed_;
    void add_gpu(const std::string& path);
    // Each GPU's wp-drm-lease global (non-desktop screens go to clients).
    struct GpuLease;
    std::vector<std::unique_ptr<GpuLease>> gpu_leases_;
    GpuLease* lease_for(const backend::Backend* gpu) const;
    std::unique_ptr<backend::Multi> backend_;
    std::unique_ptr<backend::Allocator> allocator_;
    std::unique_ptr<OutputLayout> output_layout_;
    backend::Headless* headless_ = nullptr;  // made on the first create_output() without a nested backend
    wl::Connection new_output_conn_, layout_change_conn_, backend_gone_, session_active_conn_;
    void seed_registry(const std::filesystem::path& dir);

    int wrapped_fd_ = -1;  // the wrapper's listening socket (libwayland owns it)
    pid_t startup_pid_ = -1;
    std::string startup_cmd_;               // until Xwayland is up
    bool session_target_ = false;  // atrium-session.target may be ours to stop
    std::filesystem::path session_marker_;  // "running": ours while we run
    void note_last_session();
    wl_event_source* startup_timer_ = nullptr;
    void run_startup();

    Listener<> gpu_reset_;
    // On the protocols' signals.
    std::vector<wl::Connection> connections_;
#ifdef ATRIUM_XWAYLAND
    wl::Connection xwayland_start_, xwayland_ready_;
    wl::Connection new_x11_window_, xwm_hangup_;
#endif
};

} // namespace atrium
