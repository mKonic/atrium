#include "server.hpp"

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

Server::Server(Config defaults, bool is_nested, std::filesystem::path settings_file)
    : config(defaults), nested(is_nested) {
    g_server = this;
    settings = std::make_unique<Settings>(defaults, std::move(settings_file));
    if (!settings->load())
        wlr_log(WLR_ERROR, "settings: couldn't read %s, starting from defaults",
                settings->file().c_str());
    settings->apply(config);
    setup();
}

Server::~Server() {
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

    scene = wlr_scene_create();
    root_bg = wlr_scene_rect_create(&scene->tree, 0, 0, config.background.data());
    for (auto& tree : layers_)
        tree = wlr_scene_tree_create(&scene->tree);
    drag_icons = wlr_scene_tree_create(&scene->tree);
    wlr_scene_node_place_below(&drag_icons->node, &layer(Layer::Lock)->node);
    snap_preview = std::make_unique<SnapPreview>(*this);
    overview = std::make_unique<Overview>(*this);
    switcher = std::make_unique<Switcher>(*this);
    placements = std::make_unique<Placements>(Placements::default_file());
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
    relative_pointer_manager = wlr_relative_pointer_manager_v1_create(display);
    cursor_shape_manager = wlr_cursor_shape_manager_v1_create(display, 1);
    virtual_keyboard_manager = wlr_virtual_keyboard_manager_v1_create(display);
    virtual_pointer_manager = wlr_virtual_pointer_manager_v1_create(display);

    seat = std::make_unique<Seat>(*this);

    output_manager = wlr_output_manager_v1_create(display);
    output_apply_.connect(&output_manager->events.apply,
        [this](wlr_output_configuration_v1* c) { apply_output_config(c, false); });
    output_test_.connect(&output_manager->events.test,
        [this](wlr_output_configuration_v1* c) { apply_output_config(c, true); });

    // X clients must never reach the parent X server when running nested.
    unsetenv("DISPLAY");
#ifdef ATRIUM_XWAYLAND
    // Xwayland starts lazily, on the first X client.
    xwayland = wlr_xwayland_create(display, compositor, true);
    if (xwayland) {
        xwayland_ready_.connect(&xwayland->events.ready, [this](void*) {
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
#ifdef ATRIUM_XWAYLAND
    xwayland_ready_.disconnect();
    new_xwayland_surface_.disconnect();
#endif
}

void Server::teardown() {
    ipc.reset();
    disconnect_listeners();
#ifdef ATRIUM_XWAYLAND
    wlr_xwayland_destroy(xwayland);
    xwayland = nullptr;
#endif
    shell.reset();  // stops it
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
    seat.reset();

    // wlroots needs the backend destroyed by hand before the display, or the
    // seat is used after free.
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
    if (nested)
        return;  // the host session's environment is not ours to change
    spawn("dbus-update-activation-environment --systemd WAYLAND_DISPLAY XDG_CURRENT_DESKTOP "
          "XDG_SESSION_TYPE XDG_MENU_PREFIX DISPLAY");
}

void Server::run(const char* startup_cmd) {
    const char* socket = wl_display_add_socket_auto(display);
    if (!socket)
        die("couldn't add a Wayland socket");
    setenv("WAYLAND_DISPLAY", socket, 1);
    setenv("XDG_CURRENT_DESKTOP", "atrium", 1);
    setenv("XDG_SESSION_TYPE", "wayland", 1);
    prepare_session_environment();
    install_gtk_theme();
    if (!nested)
        apply_gtk_button_layout();
    ipc = std::make_unique<Ipc>(*this, socket);

    if (!wlr_backend_start(backend))
        die("couldn't start backend");

    shell = std::make_unique<ShellProcess>(*this);
    shell->start();

    if (startup_cmd) {
        startup_pid_ = fork();
        if (startup_pid_ == 0) {
            setsid();
            execl("/bin/sh", "/bin/sh", "-c", startup_cmd, nullptr);
            _exit(127);
        }
    }

    focused_output = output_at(seat->cursor->x, seat->cursor->y);
    // Put the cursor image where the cursor actually is.
    wlr_cursor_warp_closest(seat->cursor, nullptr, seat->cursor->x, seat->cursor->y);
    seat->set_default_cursor();

    wlr_log(WLR_INFO, "running on WAYLAND_DISPLAY=%s", socket);
    wl_display_run(display);
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
        if (o->enabled())
            continue;
        auto* head = wlr_output_configuration_head_v1_create(config_out, o->wlr);
        head->state.enabled = false;
        if (o->asleep)
            continue;
        wlr_output_layout_remove(output_layout, o->wlr);
        o->box = o->usable = {};
    }
    for (Output* o : outputs) {
        if (o->enabled() && !wlr_output_layout_get(output_layout, o->wlr))
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
        if (!o->enabled())
            continue;
        auto* head = wlr_output_configuration_head_v1_create(config_out, o->wlr);

        wlr_output_layout_get_box(output_layout, o->wlr, &o->box);
        o->usable = o->box;
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
}

void Server::apply_output_config(wlr_output_configuration_v1* config_in, bool test) {
    size_t n = 0;
    wlr_backend_output_state* states = wlr_output_configuration_v1_build_state(config_in, &n);
    bool ok = false;
    wlr_output_swapchain_manager swapchains;

    if (!states) {
        wlr_output_configuration_v1_send_failed(config_in);
        wlr_output_configuration_v1_destroy(config_in);
        return;
    }

    wlr_output_swapchain_manager_init(&swapchains, backend);
    ok = wlr_output_swapchain_manager_prepare(&swapchains, states, n);
    if (ok && !test) {
        for (size_t i = 0; i < n; ++i) {
            wlr_swapchain* sc = wlr_output_swapchain_manager_get_swapchain(&swapchains, states[i].output);
            if (sc && !states[i].output->enabled)
                wlr_output_state_set_buffer(&states[i].base, wlr_swapchain_acquire(sc));
        }
        ok = wlr_backend_commit(backend, states, n);
        if (ok) {
            wlr_output_swapchain_manager_apply(&swapchains);
            wlr_output_configuration_head_v1* head;
            wl_list_for_each(head, &config_in->heads, link) {
                auto* o = static_cast<Output*>(head->state.output->data);
                o->asleep = false;
                // Re-adding at the same position would mark the output as
                // manually placed, so only move it when it actually moved.
                if (head->state.enabled &&
                    (o->box.x != head->state.x || o->box.y != head->state.y ||
                     !wlr_output_layout_get(output_layout, o->wlr)))
                    wlr_output_layout_add(output_layout, o->wlr, head->state.x, head->state.y);
            }
        }
    }

    wlr_output_swapchain_manager_finish(&swapchains);
    for (size_t i = 0; i < n; ++i)
        wlr_output_state_finish(&states[i].base);
    free(states);

    if (ok)
        wlr_output_configuration_v1_send_succeeded(config_in);
    else
        wlr_output_configuration_v1_send_failed(config_in);
    wlr_output_configuration_v1_destroy(config_in);

    update_outputs();
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

Hit Server::hit_test(double lx, double ly) const {
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
    if (!config.remember_placement || !placeable(view))
        return std::nullopt;
    const std::string_view app = view->app_id();
    // Only the first window: a second one cascades off the first as usual.
    for (const View* v : views)
        if (v != view && v->mapped && v->app_id() && app == v->app_id())
            return std::nullopt;
    if (const Placement* p = placements->find(std::string(app)))
        return *p;
    return std::nullopt;
}

void Server::remember_placement(const View* view) {
    if (!config.remember_placement || !placeable(view) || !view->output || !view->mapped)
        return;
    // The floating box, whatever state the window is in now.
    const bool away = view->maximized || view->snapped || view->fullscreen;
    const wlr_box b = away ? view->restore : view->geom;
    const wlr_box& o = view->output->box;
    placements->remember(view->app_id(), Placement{view->output->wlr->name, b.x - o.x, b.y - o.y,
                                                   b.width, b.height, view->maximized, view->snapped});
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
    }
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
    if (focused_view) {
        drop_focus();
    }
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
    case Action::SpawnTerminal: spawn(config.terminal); break;
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
    case Action::SwitchNext: switcher->step(+1, b.mods & ~uint32_t(WLR_MODIFIER_SHIFT)); break;
    case Action::SwitchPrev: switcher->step(-1, b.mods & ~uint32_t(WLR_MODIFIER_SHIFT)); break;
    case Action::CycleSpaceNext: cycle_space(+1); break;
    case Action::CycleSpacePrev: cycle_space(-1); break;
    case Action::RestartShell: if (shell) shell->restart(); break;
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
    if (!settings->save())
        wlr_log(WLR_ERROR, "settings: couldn't write %s", settings->file().c_str());

    auto is = [&](const char* prefix) { return key.starts_with(prefix); };
    if (is("appearance.blur"))
        apply_blur_settings();
    if (is("appearance.")) {
        wlr_scene_rect_set_color(root_bg, config.background.data());
        for (View* v : views)
            v->update_decorations();
        for (Output* o : outputs)
            for (auto& list : o->layers)
                for (LayerSurface* l : list)
                    l->refresh_blur();
    }
    if (key == "session.shell" && shell)
        shell->restart();
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
    wlr_scene_set_blur_data(scene, config.blur_passes, config.blur_radius, 0.02f, 0.9f, 0.9f, 1.1f);
    wlr_scene_node_set_enabled(&background_blur->node, config.blur);
    for (View* v : views)
        v->update_decorations();
    wlr_scene_optimized_blur_mark_dirty(background_blur);
}

void Server::notify_window(const View& view, const char* what) {
    if (!ipc || view.unmanaged())
        return;
    ipc->broadcast("windows", {{"event", std::string("window.") + what}, {"window", Ipc::window_json(view)}});
}

} // namespace atrium
