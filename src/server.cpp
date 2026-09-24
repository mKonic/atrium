#include "server.hpp"
#include "terminal.hpp"
#include "input_method.hpp"
#include "background_effect.hpp"
#include "session_management.hpp"
#include "toplevel_drag.hpp"
#include "paths.hpp"
#include <cstring>
#include <fstream>
#include "registry.hpp"

#include "ipc.hpp"
#include "layer_surface.hpp"
#include "output.hpp"
#include "seat.hpp"
#include "session_lock.hpp"
#include "space.hpp"
#include "settings.hpp"
#include "overview.hpp"
#include "shell_process.hpp"
#include "placements.hpp"
#include "switcher.hpp"
#include "snap_preview.hpp"
#include "theme.hpp"
#include "titlebar.hpp"
#include "view.hpp"
#include "xwayland_view.hpp"

#include <algorithm>
#include <csignal>
#include <cstdlib>
#include <ctime>
#include <sys/wait.h>
#include <unistd.h>

namespace atrium {

void report_child_exit(pid_t pid, int status);  // shell_process.cpp

namespace {

Server* g_server = nullptr;  // for the signal handler only

void handle_signal(int signo) {
    if (signo == SIGCHLD) {
        int status = 0;
        pid_t pid;
        while ((pid = waitpid(-1, &status, WNOHANG)) > 0)
            report_child_exit(pid, status);
    } else if ((signo == SIGINT || signo == SIGTERM) && g_server) {
        g_server->quit();
    }
}

[[noreturn]] void die(const char* what) {
    wlr_log(WLR_ERROR, "%s", what);
    std::exit(EXIT_FAILURE);
}

} // namespace

Server::Server(Config defaults, bool is_nested, std::filesystem::path registry_file)
    : config(defaults), nested(is_nested) {
    g_server = this;
    registry = std::make_unique<Registry>(registry_file.string());
    if (!registry->ok()) {
        wlr_log(WLR_ERROR, "registry: couldn't open %s; nothing will be remembered", registry_file.c_str());
        registry = std::make_unique<Registry>(":memory:");
    }
    settings = std::make_unique<Settings>(defaults, registry.get());
    settings->load();
    if (registry->fresh())
        seed_registry(registry_file.parent_path());
    settings->apply(config);
    rebuild_from_registry();
    setup();
}

// A new registry: what the old stores held (settings.json and
// placements.json from before the registry), or the defaults.
void Server::seed_registry(const std::filesystem::path& dir) {
    namespace fs = std::filesystem;
    auto read = [](const fs::path& f) {
        std::ifstream in(f);
        json doc = in ? json::parse(in, nullptr, false) : json();
        return doc.is_discarded() ? json() : doc;
    };
    const json old = read(dir / "settings.json");
    const char* state = std::getenv("XDG_STATE_HOME");
    const char* home = std::getenv("HOME");
    const fs::path state_dir = state && *state ? fs::path(state) : fs::path(home ? home : ".") / ".local" / "state";
    const json placed = read(state_dir / "atrium" / "placements.json");
    if (old.is_object())
        wlr_log(WLR_INFO, "registry: importing %s", (dir / "settings.json").c_str());

    registry->begin();
    settings->import(old);
    std::vector<RuleRecord> leftover;
    const auto field = [&](const char* key, json fallback) {
        return old.is_object() && old.contains(key) ? old[key] : fallback;
    };
    for (const AppRecord& a : apps_from_legacy(field("windows.rules", default_rules()),
                                               field("dock.pinned", default_dock()), placed, &leftover))
        registry->put_app(a);
    for (const RuleRecord& r : leftover)
        registry->add_rule(r);
    registry->replace_shortcuts(shortcuts_from_json(field("shortcuts.bindings", default_keybinds())));
    registry->commit();
}

// Rules and shortcuts as the compositor uses them, from the registry's
// records: pattern rules first (they say more), then one per app.
void Server::rebuild_from_registry() {
    json rules = json::array();
    auto add = [&](json r, const std::string& secret, int space, const std::string& launch,
                   const std::optional<bool>& maximized, const std::optional<bool>& fullscreen) {
        if (!secret.empty())
            r["secret"] = secret;
        if (space)
            r["space"] = space;
        if (!launch.empty() && !secret.empty())
            r["launch"] = launch;
        if (maximized)
            r["maximized"] = *maximized;
        if (fullscreen)
            r["fullscreen"] = *fullscreen;
        rules.push_back(std::move(r));
    };
    for (const RuleRecord& r : registry->rules()) {
        json m = json::object();
        if (!r.app_pattern.empty())
            m["app_id"] = r.app_pattern;
        if (!r.title_pattern.empty())
            m["title"] = r.title_pattern;
        add(m, r.secret, r.space, r.launch, r.maximized, r.fullscreen);
    }
    for (const AppRecord& a : registry->apps()) {
        if (a.secret.empty() && !a.space && !a.maximized && !a.fullscreen)
            continue;
        std::string escaped = "^";
        for (char c : a.app_id) {
            if (std::strchr(".^$|()[]{}*+?\\", c))
                escaped += '\\';
            escaped += c;
        }
        add({{"app_id", escaped + "$"}}, a.secret, a.space, a.launch, a.maximized, a.fullscreen);
    }
    config.rules = parse_rules(rules);

    json binds = json::array();
    for (const ShortcutRecord& k : registry->shortcuts())
        binds.push_back(shortcut_json(k));
    config.keybinds = resolve_keybinds(binds, config.mod);
}

Server::~Server() {
    // A clean end: the next start has nothing to report.
    if (!session_marker_.empty()) {
        std::error_code ec;
        std::filesystem::remove(session_marker_, ec);
    }
    // The session's services and autostarted apps go with it.
    if (session_target_)
        spawn("systemctl --user -q is-active atrium-session.target && systemctl --user stop --no-block atrium-session.target");
    teardown();
    g_server = nullptr;
}

void Server::setup() {
    struct sigaction sa{};
    sa.sa_flags = SA_RESTART;
    sa.sa_handler = handle_signal;
    sigemptyset(&sa.sa_mask);
    for (int sig : {SIGCHLD, SIGINT, SIGTERM, SIGPIPE})
        sigaction(sig, &sa, nullptr);

    display = wl_display_create();
    loop = wl_display_get_event_loop(display);

    // Picks DRM+libinput on a TTY, or a window when WAYLAND_DISPLAY/DISPLAY is set.
    backend = wlr_backend_autocreate(loop, &session);
    if (!backend)
        die("couldn't create backend");
    // Nested, the backend dies with the session atrium runs inside. wlroots
    // insists nothing still listens on it by then, so let go and end.
    backend_destroy_.connect(&backend->events.destroy, [this](void*) {
        wlr_log(WLR_ERROR, "the backend went away (the host session ended?); quitting");
        new_output_.disconnect();
        if (seat)
            seat->backend_gone();
        backend_destroy_.disconnect();
        backend = nullptr;
        wl_display_terminate(display);
    });

    scene = wlr_scene_create();
    root_bg = wlr_scene_rect_create(&scene->tree, 0, 0, config.background.data());
    for (auto& tree : layers_)
        tree = wlr_scene_tree_create(&scene->tree);
    drag_icons = wlr_scene_tree_create(&scene->tree);
    wlr_scene_node_place_below(&drag_icons->node, &layer(Layer::Lock)->node);
    snap_preview = std::make_unique<SnapPreview>(*this);
    overview = std::make_unique<Overview>(*this);
    switcher = std::make_unique<Switcher>(*this);
    background_blur = wlr_scene_optimized_blur_create(&scene->tree, 0, 0);
    wlr_scene_node_place_above(&background_blur->node, &layer(Layer::Bottom)->node);
    apply_blur_settings();

    // scenefx's renderer: GLES2 with rounded corners, shadows and blur.
    renderer = fx_renderer_create(backend);
    if (!renderer)
        die("couldn't create renderer");
    gpu_reset_.connect(&renderer->events.lost, [this](void*) { gpu_reset(); });

    wlr_renderer_init_wl_shm(renderer, display);
    if (wlr_renderer_get_texture_formats(renderer, WLR_BUFFER_CAP_DMABUF)) {
        wlr_drm_create(display, renderer);
        wlr_scene_set_linux_dmabuf_v1(scene,
            wlr_linux_dmabuf_v1_create_with_renderer(display, 5, renderer));
    }
    int drm_fd = wlr_renderer_get_drm_fd(renderer);
    if (drm_fd >= 0 && renderer->features.timeline && backend->features.timeline)
        wlr_linux_drm_syncobj_manager_v1_create(display, 1, drm_fd);

    allocator = wlr_allocator_autocreate(backend, renderer);
    if (!allocator)
        die("couldn't create allocator");

    compositor = wlr_compositor_create(display, 6, renderer);
    wlr_subcompositor_create(display);
    wlr_data_device_manager_create(display);
    wlr_export_dmabuf_manager_v1_create(display);
    wlr_screencopy_manager_v1_create(display);
    wlr_ext_image_copy_capture_manager_v1_create(display, 1);
    wlr_ext_output_image_capture_source_manager_v1_create(display, 1);
    wlr_data_control_manager_v1_create(display);
    wlr_ext_data_control_manager_v1_create(display, 1);
    wlr_primary_selection_v1_device_manager_create(display);
    wlr_viewporter_create(display);
    wlr_single_pixel_buffer_manager_v1_create(display);
    wlr_fractional_scale_manager_v1_create(display, 1);
    wlr_presentation_create(display, backend, 2);
    wlr_alpha_modifier_v1_create(display);

    activation = wlr_xdg_activation_v1_create(display);
    activation_request_.connect(&activation->events.request_activate,
        [this](auto* e) { activation_request(e); });

    wlr_scene_set_gamma_control_manager_v1(scene, wlr_gamma_control_manager_v1_create(display));

    power_manager = wlr_output_power_manager_v1_create(display);
    output_power_.connect(&power_manager->events.set_mode, [this](auto* e) { set_output_power(e); });

    output_layout = wlr_output_layout_create(display);
    layout_change_.connect(&output_layout->events.change, [this](void*) { update_outputs(); });
    wlr_xdg_output_manager_v1_create(display, output_layout);

    workspace_manager = wlr_ext_workspace_manager_v1_create(display, 1);
    workspace_commit_.connect(&workspace_manager->events.commit,
        [this](wlr_ext_workspace_v1_commit_event* e) { workspace_requests(e); });

    new_output_.connect(&backend->events.new_output, [this](wlr_output* o) { new_output(o); });

    xdg_shell = wlr_xdg_shell_create(display, 6);
    new_xdg_toplevel_.connect(&xdg_shell->events.new_toplevel,
        [this](wlr_xdg_toplevel* t) { new XdgView(*this, t); });
    new_xdg_popup_.connect(&xdg_shell->events.new_popup,
        [this](wlr_xdg_popup* p) { handle_new_xdg_popup(*this, p); });

    layer_shell = wlr_layer_shell_v1_create(display, 4);
    new_layer_surface_.connect(&layer_shell->events.new_surface, [this](wlr_layer_surface_v1* l) {
        if (!l->output) {
            if (!focused_output) {
                wlr_layer_surface_v1_destroy(l);
                return;
            }
            l->output = focused_output->wlr;
        }
        new LayerSurface(*this, l);
    });

    idle_notifier = wlr_idle_notifier_v1_create(display);
    idle_inhibit_manager = wlr_idle_inhibit_v1_create(display);
    new_idle_inhibitor_.connect(&idle_inhibit_manager->events.new_inhibitor,
        [this](wlr_idle_inhibitor_v1* i) { new_idle_inhibitor(i); });

    session_lock_manager = wlr_session_lock_manager_v1_create(display);
    new_lock_.connect(&session_lock_manager->events.new_lock, [this](wlr_session_lock_v1* l) {
        wlr_scene_node_set_enabled(&locked_bg->node, true);
        if (lock) {  // one lock at a time
            wlr_session_lock_v1_destroy(l);
            return;
        }
        lock = new SessionLock(*this, l);
    });
    locked_bg = wlr_scene_rect_create(layer(Layer::Lock), 0, 0, config.lock_background.data());
    wlr_scene_node_set_enabled(&locked_bg->node, false);

    ext_toplevel_list = wlr_ext_foreign_toplevel_list_v1_create(display, 1);
    toplevel_manager = wlr_foreign_toplevel_manager_v1_create(display);
    toplevel_capture_manager =
        wlr_ext_foreign_toplevel_image_capture_source_manager_v1_create(display, 1);
    new_capture_request_.connect(&toplevel_capture_manager->events.new_request,
        [this](auto* r) { new_toplevel_capture(r); });

    // atrium decorates every window that lets it, over both protocols.
    kde_decoration_manager = wlr_server_decoration_manager_create(display);
    wlr_server_decoration_manager_set_default_mode(kde_decoration_manager, WLR_SERVER_DECORATION_MANAGER_MODE_SERVER);
    // Usually arrives before the surface has its toplevel role; XdgView picks
    // those up itself when it is created.
    new_kde_decoration_.connect(&kde_decoration_manager->events.new_decoration, [](wlr_server_decoration* d) {
        wlr_xdg_surface* xdg = wlr_xdg_surface_try_from_wlr_surface(d->surface);
        if (xdg && xdg->role == WLR_XDG_SURFACE_ROLE_TOPLEVEL && xdg->data)
            static_cast<XdgView*>(xdg->data)->set_kde_decoration(d);
    });
    xdg_decoration_manager = wlr_xdg_decoration_manager_v1_create(display);
    new_decoration_.connect(&xdg_decoration_manager->events.new_toplevel_decoration,
        [](wlr_xdg_toplevel_decoration_v1* d) {
            if (auto* view = static_cast<XdgView*>(d->toplevel->base->data))
                view->set_decoration(d);
        });

    pointer_constraints = wlr_pointer_constraints_v1_create(display);
    // A client that asks (a VM, remote desktop, the Settings app recording a
    // shortcut) gets the keys atrium would otherwise take, while focused.
    shortcuts_inhibit_manager = wlr_keyboard_shortcuts_inhibit_v1_create(display);
    new_shortcuts_inhibitor_.connect(&shortcuts_inhibit_manager->events.new_inhibitor,
        [](wlr_keyboard_shortcuts_inhibitor_v1* inhibitor) {
            wlr_keyboard_shortcuts_inhibitor_v1_activate(inhibitor);
        });
    relative_pointer_manager = wlr_relative_pointer_manager_v1_create(display);
    cursor_shape_manager = wlr_cursor_shape_manager_v1_create(display, 1);
    virtual_keyboard_manager = wlr_virtual_keyboard_manager_v1_create(display);
    virtual_pointer_manager = wlr_virtual_pointer_manager_v1_create(display);
    setup_window_hints();

    seat = std::make_unique<Seat>(*this);
    input_method = std::make_unique<InputMethodRelay>(*this);
    background_effects = std::make_unique<BackgroundEffects>(*this);
    toplevel_drags = std::make_unique<ToplevelDrags>(*this);
    sessions = std::make_unique<SessionManagement>(*this);

    output_manager = wlr_output_manager_v1_create(display);
    output_apply_.connect(&output_manager->events.apply,
        [this](wlr_output_configuration_v1* c) { apply_output_config(c, false); });
    output_test_.connect(&output_manager->events.test,
        [this](wlr_output_configuration_v1* c) { apply_output_config(c, true); });

    // X clients must never reach the parent X server when running nested.
    unsetenv("DISPLAY");
#ifdef ATRIUM_XWAYLAND
    // Xwayland starts lazily, on the first X client.
    // Not lazy: an app elevated with pkexec may be the first X client, and
    // it must find root already let in (allow_root_x11), which only works
    // once Xwayland is up. A lazily started one also forgets it on restart.
    xwayland = wlr_xwayland_create(display, compositor, false);
    if (xwayland) {
        xwayland_ready_.connect(&xwayland->events.ready, [this](void*) {
            allow_root_x11(xwayland->display_name);
            run_startup();
            wlr_xwayland_set_seat(xwayland, seat->wlr);
            if (auto* xc = wlr_xcursor_manager_get_xcursor(seat->xcursor, "default", 1)) {
                auto* img = xc->images[0];
                wlr_xwayland_set_cursor(xwayland, wlr_xcursor_image_get_buffer(img),
                                        img->hotspot_x, img->hotspot_y);
            }
        });
        new_xwayland_surface_.connect(&xwayland->events.new_surface,
            [this](wlr_xwayland_surface* s) { new XwaylandView(*this, s); });
        setenv("DISPLAY", xwayland->display_name, 1);
    } else {
        wlr_log(WLR_ERROR, "failed to set up Xwayland, continuing without it");
    }
#endif
}

// Every listener must be off its signal before the object carrying the signal
// is freed: a Listener destroyed later would unlink from freed memory.
void Server::disconnect_listeners() {
    new_output_.disconnect();
    layout_change_.disconnect();
    output_apply_.disconnect();
    output_test_.disconnect();
    output_power_.disconnect();
    new_xdg_toplevel_.disconnect();
    new_xdg_popup_.disconnect();
    new_decoration_.disconnect();
    new_kde_decoration_.disconnect();
    new_layer_surface_.disconnect();
    activation_request_.disconnect();
    new_idle_inhibitor_.disconnect();
    new_lock_.disconnect();
    new_capture_request_.disconnect();
    gpu_reset_.disconnect();
    workspace_commit_.disconnect();
    new_shortcuts_inhibitor_.disconnect();
    set_icon_.disconnect();
    set_tag_.disconnect();
#ifdef ATRIUM_XWAYLAND
    xwayland_ready_.disconnect();
    new_xwayland_surface_.disconnect();
#endif
}

#ifdef ATRIUM_XWAYLAND
// Apps that elevate with pkexec (GParted and friends) lose the session's
// environment and come back as root over X, which Xwayland refuses: only
// our user may connect. Let local root in, and nobody else: the
// server-interpreted "localuser:root" entry `xhost +si:localuser:root` adds,
// done here so no xhost or exported variables are needed. Root can reach
// this session regardless; this only stops it being refused.
void Server::allow_root_x11(const char* display) {
    xcb_connection_t* conn = xcb_connect(display, nullptr);
    if (xcb_connection_has_error(conn)) {
        wlr_log(WLR_ERROR, "xwayland: couldn't connect to %s to allow root", display);
        xcb_disconnect(conn);
        return;
    }
    static constexpr char kRoot[] = "localuser\0root";
    xcb_generic_error_t* err = xcb_request_check(conn,
        xcb_change_hosts_checked(conn, XCB_HOST_MODE_INSERT, XCB_FAMILY_SERVER_INTERPRETED,
                                 sizeof kRoot - 1, reinterpret_cast<const uint8_t*>(kRoot)));
    if (err) {
        wlr_log(WLR_ERROR, "xwayland: allowing root failed (X error %d)", err->error_code);
        free(err);
    }
    xcb_disconnect(conn);
}
#endif

// The watchers that feed cliphist, the store the Super+V picker reads. A
// nested atrium leaves them to the host session (they would record into
// the same history) unless pointed at another one.
void Server::start_clipboard_history() {
    namespace fs = std::filesystem;
    if (!config.clipboard_history)
        return;
    if (nested && !std::getenv("CLIPHIST_DB_PATH"))
        return;
    for (const char* tool : {"/usr/bin/cliphist", "/usr/bin/wl-paste"})
        if (!fs::exists(tool)) {
            wlr_log(WLR_INFO, "clipboard history: %s not installed", tool);
            return;
        }
    spawn("exec wl-paste --type text --watch cliphist store");
    spawn("exec wl-paste --type image --watch cliphist store");
}

// Things the hardware forgets between boots. The monitor's brightness only
// takes a moment after login (DDC answers late), hence the pause.
void Server::restore_power_and_brightness() {
    if (nested)
        return;
    apply_power_profile();
    spawn("sleep 2; for d in $(ddcutil detect --brief 2>/dev/null | awk '/^Display/{print $2}'); do "
          "ddcutil setvcp 10 " + std::to_string(config.brightness) + " --display \"$d\" --noverify; done");
}

void Server::apply_power_profile() {
    if (!nested)
        spawn("command -v powerprofilesctl >/dev/null && exec powerprofilesctl set " + config.power_profile);
}

void Server::run_startup() {
    if (startup_timer_) {
        wl_event_source_remove(startup_timer_);
        startup_timer_ = nullptr;
    }
    if (startup_cmd_.empty())
        return;
    const std::string cmd = std::exchange(startup_cmd_, {});
    startup_pid_ = fork();
    if (startup_pid_ == 0) {
        setsid();
        execl("/bin/sh", "/bin/sh", "-c", cmd.c_str(), nullptr);
        _exit(127);
    }
}

void Server::teardown() {
    ipc.reset();
    disconnect_listeners();
#ifdef ATRIUM_XWAYLAND
    wlr_xwayland_destroy(xwayland);
    xwayland = nullptr;
#endif
    shell.reset();  // stops it
    if (startup_timer_) {
        wl_event_source_remove(startup_timer_);
        startup_timer_ = nullptr;
    }
    // Clients go first: their windows, layer surfaces and lock unwind through
    // their own destroy handlers while everything they touch still exists.
    wl_display_destroy_clients(display);
    if (startup_pid_ > 0) {
        kill(-startup_pid_, SIGTERM);
        waitpid(startup_pid_, nullptr, 0);
    }

    shutting_down = true;
    shown_secret = nullptr;
    switcher.reset();
    overview.reset();
    snap_preview.reset();
    spaces.clear();
    input_method.reset();  // hooked to the seat
    background_effects.reset();
    toplevel_drags.reset();
    sessions.reset();
    seat.reset();

    // wlroots needs the backend destroyed by hand before the display, or the
    // seat is used after free.
    backend_destroy_.disconnect();
    if (backend)  // gone already if the host session ended
        wlr_backend_destroy(backend);
    wl_display_destroy(display);
    // Only after the display: outputs are gone and no scene output is left.
    wlr_scene_node_destroy(&scene->tree.node);
}

// KDE apps build their app database from ${XDG_MENU_PREFIX}applications.menu.
// Arch ships only plasma-applications.menu, so without the prefix Dolphin's
// "Open With" is empty and no default app ever resolves. In a real session,
// also hand our environment to systemd and D-Bus, so services they start
// (KIO workers, kded, portals) see the same values.
void Server::prepare_session_environment() {
    namespace fs = std::filesystem;
    const char* prefix = std::getenv("XDG_MENU_PREFIX");
    if ((!prefix || !*prefix) && !fs::exists("/etc/xdg/menus/applications.menu") &&
        fs::exists("/etc/xdg/menus/plasma-applications.menu"))
        setenv("XDG_MENU_PREFIX", "plasma-", 1);
    // Running from the source tree: atrium's own desktop entries and icons
    // (System Settings) aren't installed, so point at them where they are.
    std::error_code ec;
    const fs::path exe = fs::read_symlink("/proc/self/exe", ec);
    if (!ec && exe.string().starts_with(ATRIUM_SOURCE_DIR)) {
        const char* dirs = std::getenv("XDG_DATA_DIRS");
        const std::string ours = std::string(ATRIUM_SOURCE_DIR) + "/data/share";
        setenv("XDG_DATA_DIRS", (ours + ":" + (dirs && *dirs ? dirs : "/usr/local/share:/usr/share")).c_str(), 1);
    }
}

void Server::run(const char* startup_cmd) {
    const char* socket = wl_display_add_socket_auto(display);
    if (!socket)
        die("couldn't add a Wayland socket");
    setenv("WAYLAND_DISPLAY", socket, 1);
    setenv("XDG_CURRENT_DESKTOP", "atrium", 1);
    setenv("XDG_SESSION_TYPE", "wayland", 1);
    prepare_session_environment();
    if (!config.greeter) {  // the greeter's user has no home to theme
        install_gtk_theme(config.light, config.accent);
        install_qt_theme(config.light, config.accent, interface());
        install_app_defaults();
    }
    if (!nested && !config.greeter) {
        // D-Bus-started apps get the session's environment, themes included.
        // (A nested atrium's environment isn't the host session's.)
        // Then the session's services and autostart apps, unless something
        // else (uwsm) already runs the graphical session.
        spawn("dbus-update-activation-environment --systemd WAYLAND_DISPLAY XDG_CURRENT_DESKTOP "
              "XDG_SESSION_TYPE XDG_MENU_PREFIX DISPLAY GTK_THEME QT_QPA_PLATFORMTHEME QTENGINE_CONFIG XDG_CONFIG_DIRS; "
              // Ours still up is a crashed atrium's: over from the start, so
              // login apps start again. Another's (uwsm's) is left alone.
              "if systemctl --user -q is-active atrium-session.target; then "
              "systemctl --user stop atrium-session.target graphical-session.target; "
              "systemctl --user start --no-block atrium-session.target; "
              "elif ! systemctl --user -q is-active graphical-session.target && "
              "systemctl --user -q cat atrium-session.target >/dev/null 2>&1; then "
              "systemctl --user start --no-block atrium-session.target; fi");
        session_target_ = true;
        apply_gtk_button_layout();
        apply_color_scheme(config.light);
        apply_accent_color(config.accent);
        apply_interface(interface());
    }
    ipc = std::make_unique<Ipc>(*this, socket);

    if (!wlr_backend_start(backend))
        die("couldn't start backend");

    shell = std::make_unique<ShellProcess>(*this);
    shell->start();
    if (!config.greeter) {
        start_clipboard_history();
        restore_power_and_brightness();
    }
    if (!nested && !config.greeter)
        note_last_session();

    if (startup_cmd && !config.greeter) {
        startup_cmd_ = startup_cmd;
#ifdef ATRIUM_XWAYLAND
        // X apps in it should find Xwayland set up (allow_root_x11); give
        // it a moment at most.
        if (xwayland) {
            startup_timer_ = wl_event_loop_add_timer(loop, [](void* data) {
                static_cast<Server*>(data)->run_startup();
                return 0;
            }, this);
            wl_event_source_timer_update(startup_timer_, 3000);
        } else
#endif
            run_startup();
    }

    focused_output = output_at(seat->cursor->x, seat->cursor->y);
    // Put the cursor image where the cursor actually is.
    wlr_cursor_warp_closest(seat->cursor, nullptr, seat->cursor->x, seat->cursor->y);
    seat->set_default_cursor();

    wlr_log(WLR_INFO, "running on WAYLAND_DISPLAY=%s", socket);
    wl_display_run(display);
}

// A marker holds our pid while we run. Still there at the next start, with
// its atrium gone, the last session crashed: say so once the shell is up.
void Server::note_last_session() {
    const char* state = std::getenv("XDG_STATE_HOME");
    const char* home = std::getenv("HOME");
    std::filesystem::path dir = state && *state ? std::filesystem::path(state) / "atrium"
                              : home ? std::filesystem::path(home) / ".local/state/atrium" : std::filesystem::path();
    if (dir.empty())
        return;
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    session_marker_ = dir / "running";
    bool crashed = false;
    if (std::ifstream in(session_marker_); in) {
        pid_t pid = 0;
        in >> pid;
        crashed = pid > 0 && pid != getpid() && kill(pid, 0) != 0 && errno == ESRCH;
    }
    std::ofstream(session_marker_) << getpid() << "\n";
    // Once the shell's notification server answers (gdbus comes with GLib,
    // which atrium needs anyway; notify-send may not be there).
    if (crashed)
        spawn("for i in $(seq 30); do gdbus call --session --dest org.freedesktop.Notifications "
              "--object-path /org/freedesktop/Notifications --method org.freedesktop.Notifications.Notify "
              "atrium 0 dialog-warning 'atrium stopped unexpectedly' "
              "'Your last session ended in a crash. For the details: coredumpctl info atrium' "
              "'[]' '{}' 10000 >/dev/null 2>&1 && break; sleep 1; done");
}

void Server::quit() {
    wl_display_terminate(display);
}

// --- outputs -----------------------------------------------------------------

void Server::new_output(wlr_output* wlr) {
    if (!wlr_output_init_render(wlr, allocator, renderer))
        return;
    auto* output = new Output(*this, wlr);
    outputs.push_back(output);
    output_added(output);
    restore_display(output);
    // Joining the layout (in Output's constructor) ran update_outputs()
    // before this output was in `outputs`: its box, and the bar's and every
    // panel's room, come from here. (A nested output gets a resize from its
    // host window soon after, which hid this; a real screen gets nothing.)
    update_outputs();
}

Output* Server::output_at(double lx, double ly) const {
    wlr_output* o = wlr_output_layout_output_at(output_layout, lx, ly);
    return o ? static_cast<Output*>(o->data) : nullptr;
}

// Called whenever the layout changes: an output appears, disappears, changes
// mode or position. Recomputes every box that depends on outputs and publishes
// the new state to wlr-output-management clients.
void Server::update_outputs() {
    auto* config_out = wlr_output_configuration_v1_create();

    // Disabled outputs leave the layout first, so the cursor cannot enter them.
    for (Output* o : outputs) {
        if (o->enabled() || o->dying)  // leaving: already out of the layout
            continue;
        auto* head = wlr_output_configuration_head_v1_create(config_out, o->wlr);
        head->state.enabled = false;
        if (o->asleep)
            continue;
        wlr_output_layout_remove(output_layout, o->wlr);
        o->box = o->usable = {};
    }
    for (Output* o : outputs) {
        if (o->enabled() && !o->dying && !wlr_output_layout_get(output_layout, o->wlr))
            wlr_output_layout_add_auto(output_layout, o->wlr);
    }

    wlr_output_layout_get_box(output_layout, nullptr, &layout_box);
    wlr_scene_node_set_position(&background_blur->node, layout_box.x, layout_box.y);
    wlr_scene_optimized_blur_set_size(background_blur, layout_box.width, layout_box.height);
    wlr_scene_optimized_blur_mark_dirty(background_blur);
    wlr_scene_node_set_position(&root_bg->node, layout_box.x, layout_box.y);
    wlr_scene_rect_set_size(root_bg, layout_box.width, layout_box.height);
    wlr_scene_node_set_position(&locked_bg->node, layout_box.x, layout_box.y);
    wlr_scene_rect_set_size(locked_bg, layout_box.width, layout_box.height);

    for (Output* o : outputs) {
        if (!o->enabled() || o->dying)
            continue;
        auto* head = wlr_output_configuration_head_v1_create(config_out, o->wlr);

        wlr_output_layout_get_box(output_layout, o->wlr, &o->box);
        o->usable = o->box;
        if (o->scene_output)
            wlr_scene_output_set_position(o->scene_output, o->box.x, o->box.y);
        wlr_scene_node_set_position(&o->fullscreen_bg->node, o->box.x, o->box.y);
        wlr_scene_rect_set_size(o->fullscreen_bg, o->box.width, o->box.height);

        if (o->lock_surface) {
            auto* tree = static_cast<wlr_scene_tree*>(o->lock_surface->surface->data);
            wlr_scene_node_set_position(&tree->node, o->box.x, o->box.y);
            wlr_session_lock_surface_v1_configure(o->lock_surface, o->box.width, o->box.height);
        }

        o->arrange_layers();
        o->refit_views();

        head->state.x = o->box.x;
        head->state.y = o->box.y;

        if (!focused_output)
            focused_output = o;
    }

    // Views whose output went away move to the focused one.
    if (focused_output && focused_output->enabled()) {
        for (View* v : views) {
            if (!v->output || !v->output->enabled()) {
                int x = std::clamp(v->geom.x, focused_output->usable.x,
                                   focused_output->usable.x + focused_output->usable.width - 64);
                int y = std::clamp(v->geom.y, focused_output->usable.y,
                                   focused_output->usable.y + focused_output->usable.height - 64);
                v->move_to(x, y);
            }
        }
        if (!locked)
            focus_top();
        if (focused_output->lock_surface)
            seat->keyboard_enter(focused_output->lock_surface->surface);
    }

    // The cursor image can end up at 0,0 after outputs come back; re-place it.
    wlr_cursor_move(seat->cursor, nullptr, 0, 0);

    wlr_output_manager_v1_set_configuration(output_manager, config_out);
    if (ipc)
        ipc->broadcast("outputs", {{"event", "outputs.changed"}});
}

void Server::set_output_power(wlr_output_power_v1_set_mode_event* event) {
    auto* o = static_cast<Output*>(event->output->data);
    if (!o)
        return;
    wlr_output_state state;
    wlr_output_state_init(&state);
    wlr_output_state_set_enabled(&state, event->mode);
    wlr_output_commit_state(o->wlr, &state);
    wlr_output_state_finish(&state);
    o->asleep = !event->mode;
    update_outputs();
}

void Server::gpu_reset() {
    wlr_renderer* old_renderer = renderer;
    wlr_allocator* old_allocator = allocator;

    renderer = fx_renderer_create(backend);
    if (!renderer)
        die("couldn't recreate renderer");
    allocator = wlr_allocator_autocreate(backend, renderer);
    if (!allocator)
        die("couldn't recreate allocator");

    gpu_reset_.connect(&renderer->events.lost, [this](void*) { gpu_reset(); });
    wlr_compositor_set_renderer(compositor, renderer);
    for (Output* o : outputs)
        wlr_output_init_render(o->wlr, allocator, renderer);

    wlr_allocator_destroy(old_allocator);
    wlr_renderer_destroy(old_renderer);
}

// --- focus and hit testing -------------------------------------------------

Owner Server::owner_of(wlr_surface* surface) {
    Owner owner;
    if (!surface)
        return owner;
    wlr_surface* root = wlr_surface_get_root_surface(surface);

#ifdef ATRIUM_XWAYLAND
    if (auto* xs = wlr_xwayland_surface_try_from_wlr_surface(root)) {
        owner.view = static_cast<View*>(xs->data);
        return owner;
    }
#endif
    if (auto* ls = wlr_layer_surface_v1_try_from_wlr_surface(root)) {
        owner.layer = static_cast<LayerSurface*>(ls->data);
        return owner;
    }

    // Walk popup parents up to the toplevel or layer surface they belong to.
    wlr_xdg_surface* xdg = wlr_xdg_surface_try_from_wlr_surface(root);
    while (xdg) {
        switch (xdg->role) {
        case WLR_XDG_SURFACE_ROLE_POPUP: {
            if (!xdg->popup || !xdg->popup->parent)
                return owner;
            wlr_xdg_surface* parent = wlr_xdg_surface_try_from_wlr_surface(xdg->popup->parent);
            if (!parent)
                return owner_of(xdg->popup->parent);
            xdg = parent;
            break;
        }
        case WLR_XDG_SURFACE_ROLE_TOPLEVEL:
            owner.view = static_cast<View*>(xdg->data);
            return owner;
        case WLR_XDG_SURFACE_ROLE_NONE:
            return owner;
        }
    }
    return owner;
}

Hit Server::hit_test(double lx, double ly, View* through) const {
    // Out of the scene for the lookup only; nothing draws in between.
    const bool hide = through && through->tree && through->tree->node.enabled;
    if (hide)
        wlr_scene_node_set_enabled(&through->tree->node, false);
    Hit hit = hit_test_scene(lx, ly);
    if (hide)
        wlr_scene_node_set_enabled(&through->tree->node, true);
    return hit;
}

Hit Server::hit_test_scene(double lx, double ly) const {
    Hit hit;
    const int lowest = locked ? int(Layer::Lock) : 0;
    for (int l = kLayerCount - 1; l >= lowest && !hit.surface; --l) {
        if (l == int(Layer::InputPopup))
            continue;
        wlr_scene_node* node = wlr_scene_node_at(&layers_[l]->node, lx, ly, &hit.sx, &hit.sy);
        if (!node || (node->type != WLR_SCENE_NODE_BUFFER && node->type != WLR_SCENE_NODE_RECT))
            continue;
        if (node->type == WLR_SCENE_NODE_RECT) {
            // Only a secret space's backdrop is a rect that carries data.
            if (node->data) {
                hit.backdrop = static_cast<Space*>(node->data);
                return hit;
            }
            continue;
        }
        if (auto* ss = wlr_scene_surface_try_from_buffer(wlr_scene_buffer_from_node(node))) {
            hit.surface = ss->surface;
        } else if (node->data) {
            // The only non-surface buffers carrying data are title bars.
            hit.titlebar = static_cast<Titlebar*>(node->data);
            hit.view = &hit.titlebar->view();
            return hit;
        }
    }
    Owner owner = owner_of(hit.surface);
    hit.view = owner.view;
    hit.layer = owner.layer;
    return hit;
}

// --- remembered placements ---------------------------------------------------------

namespace {

bool placeable(const View* v) {
    return !v->unmanaged() && !v->parent() && !v->is_dialog() && v->app_id() && *v->app_id();
}

} // namespace

std::optional<Placement> Server::placement_for(const View* view) const {
    // The app asked for this window back (xdg-session-management), by name.
    if (sessions)
        if (const SessionWindow* w = sessions->restoring(view))
            return w->placement;
    if (!config.remember_placement || !placeable(view))
        return std::nullopt;
    const std::string_view app = view->app_id();
    // Only the first window: a second one cascades off the first as usual.
    for (const View* v : views)
        if (v != view && v->mapped && v->app_id() && app == v->app_id())
            return std::nullopt;
    if (auto a = registry->app(std::string(app)))
        return a->placement;
    return std::nullopt;
}

Placement Server::placement_of(const View* view) const {
    // The floating box, whatever state the window is in now.
    const bool away = view->maximized || view->snapped || view->fullscreen;
    const wlr_box b = away ? view->restore : view->geom;
    const wlr_box o = view->output ? view->output->box : wlr_box{};
    return Placement{view->output ? view->output->wlr->name : "", b.x - o.x, b.y - o.y, b.width, b.height,
                     view->maximized, view->snapped};
}

void Server::remember_placement(const View* view) {
    if (sessions)
        sessions->view_unmapping(view);
    // A secret space's size belongs to the space, not the app.
    if (!config.remember_placement || !placeable(view) || !view->output || !view->mapped ||
        (view->space && view->space->secret))
        return;
    AppRecord a = registry->app(view->app_id()).value_or(AppRecord{.app_id = view->app_id()});
    a.placement = placement_of(view);
    registry->put_app(a);
}

View* Server::top_view(Output* output) const {
    // A secret space showing on the output sits above everything there.
    const Space* only = (shown_secret && (!output || shown_secret->output == output)) ? shown_secret : nullptr;
    for (View* v : views)
        if (v->visible() && (!output || v->output == output) && (!only || v->space == only))
            return v;
    return nullptr;
}

void Server::focus_top() {
    focus_view(top_view(focused_output));
}

void Server::focus_view(View* view, bool raise) {
    if (locked)
        return;
    // A window waiting on a modal dialog hands focus to the dialog (the
    // newest one, and on down a chain of them), raised along with it.
    for (bool found = true; view && found;) {
        found = false;
        for (View* v : views)
            if (v != view && v->mapped && v->parent() == view && v->modal()) {
                if (raise)
                    view->raise();
                view = v;
                found = true;
                break;
            }
    }

    // Focusing a window on another space goes there, like macOS.
    if (view && view->space && !view->space->shown())
        reveal(view->space);

    if (view && raise)
        view->raise();

    wlr_surface* old = seat->wlr->keyboard_state.focused_surface;
    if (view && view->surface() == old)
        return;

    Owner old_owner = owner_of(old);
    if (old_owner.view && old_owner.view->kind == View::Kind::Xdg)
        static_cast<XdgView*>(old_owner.view)->dismiss_popups();

    if (view && !view->unmanaged()) {
        std::erase(views, view);
        views.insert(views.begin(), view);
        if (view->output)
            focused_output = view->output;
        view->urgent = false;
    }

    // A top/overlay layer surface holding exclusive keyboard focus (a lock
    // screen stand-in, a launcher) keeps it; the view only moves up the order.
    if (old_owner.layer && old_owner.layer->mapped && old_owner.layer->wants_exclusive_keyboard() &&
        old_owner.layer->wlr->current.layer >= ZWLR_LAYER_SHELL_V1_LAYER_TOP)
        return;

    if (focused_view && focused_view != view && !(view && view->unmanaged())) {
        drop_focus();
    }

    if (!view) {
        seat->clear_keyboard_focus();
        return;
    }

    seat->refresh_pointer();
    seat->keyboard_enter(view->surface());
    view->set_activated(true);
    if (!view->unmanaged()) {
        focused_view = view;
        notify_window(*view, "focused");
        // Each window types in the layout it was left in (new ones: the first).
        if (config.layout_per_window && seat->layout() != view->keyboard_layout)
            seat->set_layout(view->keyboard_layout);
    }
}

Interface Server::interface() const {
    return {config.icon_theme, config.font, config.font_size, config.mono_font, config.cursor_theme,
            config.cursor_size};
}

// An app's global shortcut pressed or let go: the portal backend hears it
// and tells the app (push-to-talk needs both).
void Server::portal_shortcut(const std::string& arg, bool pressed) {
    const size_t slash = arg.find('/');
    if (!ipc || slash == std::string::npos)
        return;
    ipc->broadcast("portal", {{"event", pressed ? "shortcut.activated" : "shortcut.deactivated"},
                              {"app", arg.substr(0, slash)}, {"id", arg.substr(slash + 1)}});
}

void Server::keyboard_layout_changed() {
    if (config.layout_per_window && focused_view)
        focused_view->keyboard_layout = seat->layout();
    if (ipc)
        ipc->broadcast("keyboard", {{"event", "keyboard.changed"}, {"keyboard", Ipc::keyboard_json(*this)}});
}

void Server::drop_focus() {
    View* old = focused_view;
    if (!old)
        return;
    old->set_activated(false);
    focused_view = nullptr;
    notify_window(*old, "changed");
}

void Server::focus_layer(LayerSurface* layer) {
    if (locked)
        return;
    // The shell's menus and panels above windows take the keyboard but, as
    // on a Mac, the window stays the active one under them (its title bar,
    // the bar's app name). The desktop below windows is another matter: it
    // takes focus the way an app does.
    if (focused_view && layer->wlr->current.layer < ZWLR_LAYER_SHELL_V1_LAYER_TOP)
        drop_focus();
    seat->keyboard_enter(layer->wlr->surface);
}

void Server::cycle_focus(int direction) {
    // `views` is in focus order, so "next" is the least recently used window
    // on this output and "previous" undoes it.
    // With a secret space up, only its windows are reachable.
    const Space* only = (shown_secret && shown_secret->output == focused_output) ? shown_secret : nullptr;
    std::vector<View*> candidates;
    for (View* v : views)
        if (v->visible() && v->output == focused_output && !v->fullscreen && (!only || v->space == only))
            candidates.push_back(v);
    if (candidates.size() < 2)
        return;
    focus_view(direction > 0 ? candidates.back() : candidates[1]);
}

void Server::activation_request(wlr_xdg_activation_v1_request_activate_event* event) {
    Owner owner = owner_of(event->surface);
    View* view = owner.view;
    if (!view || view == focused_view)
        return;

    // A token minted from real user input (a click on a link, a notification)
    // may take focus; anything else only marks the window as wanting attention.
    if (event->token && event->token->seat) {
        if (view->minimized)
            view->set_minimized(false);
        focus_view(view);
    } else {
        view->urgent = true;
    }
}

void Server::new_toplevel_capture(
    wlr_ext_foreign_toplevel_image_capture_source_manager_v1_request* request) {
    auto* view = static_cast<View*>(request->toplevel_handle->data);
    if (!view || !view->capture_scene_)
        return;
    if (!view->capture_source_) {
        view->capture_source_ = wlr_ext_image_capture_source_v1_create_with_scene_node(
            &view->capture_scene_->tree.node, loop, allocator, renderer);
        if (!view->capture_source_)
            return;
        View::CaptureImpl& ci = view->capture_impl_;
        ci.base = view->capture_source_->impl;
        ci.impl = *ci.base;
        ci.node = &view->capture_scene_->tree.node;
        // Off and on again damages all of it.
        static constexpr auto redraw = [](wlr_scene_node* node) {
            wlr_scene_node_set_enabled(node, false);
            wlr_scene_node_set_enabled(node, true);
        };
        ci.refresh = wl_event_loop_add_timer(loop, [](void* data) {
            redraw(static_cast<View::CaptureImpl*>(data)->node);
            return 0;
        }, &ci);
        ci.impl.request_frame = [](wlr_ext_image_capture_source_v1* source, bool schedule_frame) {
            const auto* ci = reinterpret_cast<const View::CaptureImpl*>(source->impl);
            // Owed: a new session's first, or one drawn while the client
            // wasn't asking.
            if (schedule_frame)
                redraw(ci->node);
            else if (ci->refresh)
                wl_event_source_timer_update(ci->refresh, 1000);
            ci->base->request_frame(source, schedule_frame);
        };
        view->capture_source_->impl = &ci.impl;
    }
    wlr_ext_foreign_toplevel_image_capture_source_manager_v1_request_accept(
        request, view->capture_source_);
    view->send_suspended(false);
}

// --- idle ----------------------------------------------------------------------

void Server::new_idle_inhibitor(wlr_idle_inhibitor_v1* inhibitor) {
    // Owned by the inhibitor's lifetime; freed on its destroy signal.
    struct Watch {
        Listener<wlr_surface> destroy;
    };
    auto* watch = new Watch;
    watch->destroy.connect(&inhibitor->events.destroy, [this, watch](wlr_surface* surface) {
        // The inhibitor is still in the manager's list here: exclude it.
        check_idle_inhibitors(wlr_surface_get_root_surface(surface));
        delete watch;
    });
    check_idle_inhibitors();
}

void Server::check_idle_inhibitors(wlr_surface* exclude) {
    bool inhibited = false;
    wlr_idle_inhibitor_v1* inhibitor;
    wl_list_for_each(inhibitor, &idle_inhibit_manager->inhibitors, link) {
        wlr_surface* surface = wlr_surface_get_root_surface(inhibitor->surface);
        auto* tree = static_cast<wlr_scene_tree*>(surface->data);
        int x, y;
        if (exclude != surface &&
            (config.idle_inhibit_ignore_visibility || !tree ||
             wlr_scene_node_coords(&tree->node, &x, &y))) {
            inhibited = true;
            break;
        }
    }
    wlr_idle_notifier_v1_set_inhibited(idle_notifier, inhibited);
}

// --- processes -------------------------------------------------------------------

void Server::spawn(const std::string& command) {
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        // The child must not inherit our handlers.
        struct sigaction sa{};
        sa.sa_handler = SIG_DFL;
        sigemptyset(&sa.sa_mask);
        for (int sig : {SIGCHLD, SIGINT, SIGTERM, SIGPIPE})
            sigaction(sig, &sa, nullptr);
        dup2(STDERR_FILENO, STDOUT_FILENO);
        execl("/bin/sh", "/bin/sh", "-c", command.c_str(), nullptr);
        _exit(127);
    } else if (pid < 0) {
        wlr_log_errno(WLR_ERROR, "fork failed for '%s'", command.c_str());
    }
}

void Server::change_vt(unsigned vt) {
    if (session)
        wlr_session_change_vt(session, vt);
}

void Server::run_action(const Keybind& b) {
    View* v = focused_view;
    switch (b.action) {
    case Action::Spawn: spawn(b.arg); break;
    case Action::SpawnTerminal: spawn(config.terminal.empty() ? default_terminal_command() : config.terminal); break;
    case Action::CloseWindow: if (v) v->close(); break;
    case Action::ToggleFullscreen: if (v) v->set_fullscreen(!v->fullscreen); break;
    case Action::ToggleMaximize: if (v && !v->fullscreen) v->set_maximized(!v->maximized); break;
    case Action::Minimize: if (v) v->set_minimized(true); break;
    case Action::FocusNext: cycle_focus(+1); break;
    case Action::FocusPrev: cycle_focus(-1); break;
    case Action::SwitchVt: change_vt(unsigned(b.iarg)); break;
    case Action::Quit: quit(); break;
    case Action::SnapLeft: if (v) v->snap(WLR_EDGE_LEFT); break;
    case Action::SnapRight: if (v) v->snap(WLR_EDGE_RIGHT); break;
    case Action::Restore:
        if (v && v->fullscreen)
            v->set_fullscreen(false);
        else if (v && v->maximized)
            v->set_maximized(false);
        else if (v && v->snapped)
            v->unsnap(true);
        break;
    case Action::Space: switch_space(focused_output, b.iarg); break;
    case Action::MoveToSpace:
        if (v && focused_output)
            move_to_space(v, ensure_space(focused_output, b.iarg));
        break;
    case Action::SpacePrev: step_space(-1); break;
    case Action::SpaceNext: step_space(+1); break;
    case Action::Overview: overview->toggle(); break;
    case Action::AppExpose: {
        std::string app = b.arg;
        if (app.empty() || app.starts_with("window:")) {
            const View* of = v;
            if (!app.empty()) {
                of = nullptr;
                const uint64_t id = std::strtoull(app.c_str() + 7, nullptr, 10);
                for (View* w : views)
                    if (w->id == id)
                        of = w;
            }
            app = of && of->app_id() ? of->app_id() : "";
        }
        overview->open_app(app);
        break;
    }
    case Action::SwitchNext: switcher->step(+1, b.mods & ~uint32_t(WLR_MODIFIER_SHIFT)); break;
    case Action::SwitchPrev: switcher->step(-1, b.mods & ~uint32_t(WLR_MODIFIER_SHIFT)); break;
    case Action::CycleSpaceNext: cycle_space(+1); break;
    case Action::CycleSpacePrev: cycle_space(-1); break;
    case Action::RestartShell: if (shell) shell->restart(); break;
    case Action::FocusDirection:
        if (View* next = neighbor_of(v, direction_from(b.arg)))
            focus_view(next);
        break;
    case Action::MoveDirection: if (v) move_direction(v, direction_from(b.arg)); break;
    case Action::MoveToSpacePrev:
    case Action::MoveToSpaceNext:
        if (v && v->space && !v->space->secret && focused_output) {
            const int n = v->space->number + (b.action == Action::MoveToSpaceNext ? 1 : -1);
            if (n >= 1) {
                move_to_space(v, ensure_space(focused_output, n));
                switch_space(focused_output, n);
                focus_view(v);
            }
        }
        break;
    case Action::ToggleFloating:
        if (v && v->space && v->space->tiled) {
            v->float_in_tiling = !v->float_in_tiling;
            if (v->float_in_tiling)
                v->untile();
            retile(v->space);
        }
        break;
    case Action::NextLayout: seat->set_layout(seat->layout() + 1); break;
    case Action::Portal: portal_shortcut(b.arg, true); break;
    case Action::TogglePin:
        if (v) {
            v->sticky = !v->sticky;
            notify_window(*v, "changed");
        }
        break;
    case Action::ToggleTiling:
        if (focused_output && focused_output->active && !(shown_secret && shown_secret->output == focused_output))
            toggle_tiling(focused_output->active);
        break;
    case Action::Shell:
        // The shell listens on the IPC socket; it decides what "launcher" means.
        if (ipc)
            ipc->broadcast("shell", {{"event", "shell.action"}, {"name", b.arg}});
        break;
    case Action::ToggleSecret: toggle_secret(b.arg); break;
    case Action::MoveToSecret:
        if (v) {
            Space* s = ensure_secret(b.arg);
            if (v->space == s) {
                // Already there: send it back to the space underneath.
                if (focused_output && focused_output->active)
                    move_to_space(v, focused_output->active);
            } else {
                move_to_space(v, s);
            }
        }
        break;
    }
}

// --- settings and events ----------------------------------------------------------------

void Server::setting_changed(const std::string& key) {
    settings->apply(config);
    rebuild_from_registry();  // the modifier key changes what shortcuts mean

    auto is = [&](const char* prefix) { return key.starts_with(prefix); };
    if (is("appearance.blur") || key == "appearance.liquid_glass")
        apply_blur_settings();
    if ((is("appearance.blur") || key == "appearance.transparency") && background_effects)
        background_effects->announce();
    if (key == "appearance.style" || key == "appearance.accent") {
        install_gtk_theme(config.light, config.accent);  // apps opened from now on
        install_qt_theme(config.light, config.accent, interface());   // Qt apps too, live (qtengine watches it)
        if (!nested) {
            // The rest, live, through the portal.
            apply_color_scheme(config.light);
            apply_accent_color(config.accent);
        }
    }
    if (key == "appearance.icon_theme" || is("appearance.font") || key == "appearance.monospace_font" ||
        is("cursor.")) {
        install_qt_theme(config.light, config.accent, interface());
        if (!nested)
            apply_interface(interface());
    }
    if (is("appearance.")) {
        wlr_scene_rect_set_color(root_bg, config.background.data());
        for (View* v : views) {
            v->update_decorations();
            if (v->titlebar)
                v->titlebar->update();
        }
        for (Output* o : outputs)
            for (auto& list : o->layers)
                for (LayerSurface* l : list)
                    l->refresh_blur();
    }
    if (key == "power.profile")
        apply_power_profile();
    if (key == "session.shell" && shell)
        shell->restart();
    if (key == "windows.fullscreen_space" && !config.fullscreen_space)
        for (View* v : views)
            v->fullscreen_home = 0;
    if (key == "windows.tiled_titlebars")
        for (View* v : views)
            v->refresh_tiled_titlebar();
    if (is("keyboard."))
        seat->apply_keyboard_config();
    if (is("pointer.") || is("touchpad."))
        seat->apply_pointer_config();
    if (is("cursor.")) {
        seat->apply_cursor_theme();
        seat->set_default_cursor();
    }

    if (ipc)
        ipc->broadcast("settings", {{"event", "setting.changed"}, {"key", key}, {"value", settings->get(key)}});
}

void Server::apply_blur_settings() {
    // Liquid Glass lets the colours behind through, brighter and richer;
    // frosted glass dims and greys them a little.
    if (config.liquid_glass)
        wlr_scene_set_blur_data(scene, config.blur_passes, config.blur_radius, 0.01f, 1.02f, 0.95f, 1.45f);
    else
        wlr_scene_set_blur_data(scene, config.blur_passes, config.blur_radius, 0.02f, 0.9f, 0.9f, 1.1f);
    wlr_scene_node_set_enabled(&background_blur->node, config.blur);
    for (View* v : views)
        v->update_decorations();
    wlr_scene_optimized_blur_mark_dirty(background_blur);
}

void Server::show_window_menu(const View* view, double lx, double ly) {
    Output* o = output_at(lx, ly);
    if (!ipc || !view || view->unmanaged() || !o)
        return;
    ipc->broadcast("windows", {{"event", "window.menu"},
                               {"window", Ipc::window_json(*view)},
                               {"output", o->wlr->name},
                               {"x", int(lx) - o->box.x},
                               {"y", int(ly) - o->box.y}});
}

void Server::notify_window(const View& view, const char* what) {
    if (sessions && !view.unmanaged())
        sessions->view_changed(&view);
    if (!ipc || view.unmanaged())
        return;
    ipc->broadcast("windows", {{"event", std::string("window.") + what}, {"window", Ipc::window_json(view)}});
}

} // namespace atrium
