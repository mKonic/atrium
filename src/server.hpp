#pragma once
#include "config.hpp"
#include "listener.hpp"

#include <filesystem>

#include <memory>
#include <string>
#include <vector>

namespace atrium {

class Ipc;
class LayerSurface;
class Output;
class Titlebar;
class Seat;
class SessionLock;
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
    Unmanaged,   // X11 override-redirect: menus, tooltips
    Overlay,
    InputPopup,
    Lock,
    Count,
};
constexpr int kLayerCount = int(Layer::Count);

// What is under a point in layout coordinates.
struct Hit {
    wlr_surface* surface = nullptr;
    View* view = nullptr;
    LayerSurface* layer = nullptr;
    Titlebar* titlebar = nullptr;  // atrium's own title bar (then surface is null)
    Space* backdrop = nullptr;     // the dimmed screen behind a shown secret space
    double sx = 0, sy = 0;         // surface- or title-bar-local
};

// Which atrium object a wlr_surface belongs to. Popups resolve to the
// toplevel or layer surface they hang off.
struct Owner {
    View* view = nullptr;
    LayerSurface* layer = nullptr;
    explicit operator bool() const { return view || layer; }
};

class Server {
public:
    Server(Config defaults, bool nested, std::filesystem::path settings_file);
    ~Server();
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    void run(const char* startup_cmd);
    void quit();

    wlr_scene_tree* layer(Layer l) const { return layers_[int(l)]; }

    Output* output_at(double lx, double ly) const;
    Hit hit_test(double lx, double ly) const;
    static Owner owner_of(wlr_surface* surface);

    // Focus. `raise` also brings the view to the top of the stack.
    void focus_view(View* view, bool raise = true);
    void focus_layer(LayerSurface* layer);
    void focus_top();
    View* top_view(Output* output) const;
    void cycle_focus(int direction);

    // --- spaces --------------------------------------------------------------
    Space* find_space(Output* output, int number) const;
    Space* ensure_space(Output* output, int number);
    Space* find_secret(const std::string& name) const;
    Space* ensure_secret(const std::string& name);
    void switch_space(Output* output, int number);
    void step_space(int direction);
    void move_to_space(View* view, Space* space);
    void toggle_secret(const std::string& name);
    void hide_secret();
    // Make `space` visible: switch to it, or show it if it is secret.
    void reveal(Space* space);
    // Delete a numbered space nobody is looking at and nothing lives in.
    void prune_space(Space* space);
    void spaces_changed();
    // Decide a new window's space from the rules; returns what else they ask for.
    RuleResult assign_space(View* view);
    void output_added(Output* output);
    void output_removing(Output* output);

    void update_outputs();
    void check_idle_inhibitors(wlr_surface* exclude = nullptr);
    void spawn(const std::string& command);
    void change_vt(unsigned vt);
    void run_action(const Keybind& bind);

    // A setting changed through the store: refresh `config`, persist, apply
    // the side effects and tell subscribers.
    void setting_changed(const std::string& key);

    // Tell IPC subscribers about a window event ("opened", "closed",
    // "changed", "focused").
    void notify_window(const View& view, const char* what);

    Config config;
    const bool nested;

    wl_display* display = nullptr;
    wl_event_loop* loop = nullptr;
    wlr_backend* backend = nullptr;
    wlr_session* session = nullptr;
    wlr_renderer* renderer = nullptr;
    wlr_allocator* allocator = nullptr;
    wlr_compositor* compositor = nullptr;
    wlr_scene* scene = nullptr;
    wlr_scene_tree* drag_icons = nullptr;
    wlr_scene_rect* root_bg = nullptr;
    wlr_scene_rect* locked_bg = nullptr;

    wlr_output_layout* output_layout = nullptr;
    wlr_box layout_box{};
    wlr_output_manager_v1* output_manager = nullptr;
    wlr_output_power_manager_v1* power_manager = nullptr;

    wlr_xdg_shell* xdg_shell = nullptr;
    wlr_layer_shell_v1* layer_shell = nullptr;
    wlr_xdg_activation_v1* activation = nullptr;
    wlr_xdg_decoration_manager_v1* xdg_decoration_manager = nullptr;
    wlr_server_decoration_manager* kde_decoration_manager = nullptr;
    wlr_idle_notifier_v1* idle_notifier = nullptr;
    wlr_idle_inhibit_manager_v1* idle_inhibit_manager = nullptr;
    wlr_session_lock_manager_v1* session_lock_manager = nullptr;
    wlr_ext_foreign_toplevel_list_v1* ext_toplevel_list = nullptr;
    wlr_foreign_toplevel_manager_v1* toplevel_manager = nullptr;
    wlr_ext_foreign_toplevel_image_capture_source_manager_v1* toplevel_capture_manager = nullptr;
    wlr_pointer_constraints_v1* pointer_constraints = nullptr;
    wlr_relative_pointer_manager_v1* relative_pointer_manager = nullptr;
    wlr_cursor_shape_manager_v1* cursor_shape_manager = nullptr;
    wlr_virtual_keyboard_manager_v1* virtual_keyboard_manager = nullptr;
    wlr_virtual_pointer_manager_v1* virtual_pointer_manager = nullptr;
#ifdef ATRIUM_XWAYLAND
    wlr_xwayland* xwayland = nullptr;
#endif

    std::unique_ptr<Settings> settings;
    std::unique_ptr<Ipc> ipc;
    std::unique_ptr<Seat> seat;
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
    wlr_ext_workspace_manager_v1* workspace_manager = nullptr;

    // Set during teardown: objects dying with the backend skip relayout.
    bool shutting_down = false;

private:
    void setup();
    void teardown();
    void new_output(wlr_output* wlr);
    void apply_output_config(wlr_output_configuration_v1* config, bool test);
    void set_output_power(wlr_output_power_v1_set_mode_event* event);
    void activation_request(wlr_xdg_activation_v1_request_activate_event* event);
    void new_toplevel_capture(wlr_ext_foreign_toplevel_image_capture_source_manager_v1_request* request);
    void new_idle_inhibitor(wlr_idle_inhibitor_v1* inhibitor);
    void gpu_reset();
    void disconnect_listeners();
    void workspace_requests(wlr_ext_workspace_v1_commit_event* event);

    wlr_scene_tree* layers_[kLayerCount]{};
    pid_t startup_pid_ = -1;

    Listener<wlr_output> new_output_;
    Listener<> layout_change_;
    Listener<wlr_output_configuration_v1> output_apply_, output_test_;
    Listener<wlr_output_power_v1_set_mode_event> output_power_;
    Listener<wlr_xdg_toplevel> new_xdg_toplevel_;
    Listener<wlr_xdg_popup> new_xdg_popup_;
    Listener<wlr_xdg_toplevel_decoration_v1> new_decoration_;
    Listener<wlr_server_decoration> new_kde_decoration_;
    Listener<wlr_layer_surface_v1> new_layer_surface_;
    Listener<wlr_xdg_activation_v1_request_activate_event> activation_request_;
    Listener<wlr_idle_inhibitor_v1> new_idle_inhibitor_;
    Listener<wlr_session_lock_v1> new_lock_;
    Listener<wlr_ext_foreign_toplevel_image_capture_source_manager_v1_request> new_capture_request_;
    Listener<> gpu_reset_;
    Listener<wlr_ext_workspace_v1_commit_event> workspace_commit_;
#ifdef ATRIUM_XWAYLAND
    Listener<> xwayland_ready_;
    Listener<wlr_xwayland_surface> new_xwayland_surface_;
#endif
};

} // namespace atrium
