#include "server.hpp"
#include "util/log.hpp"
#include "wl/drm_lease.hpp"
#include "backend/drm/drm.hpp"
#include "backend/session.hpp"
#include "backend/wayland.hpp"
#include "input/libinput.hpp"
#include "backend/wlr.hpp"
#include "capture_state.hpp"
#include "stacking.hpp"
#include "render/renderer.hpp"
#include "terminal.hpp"
#include "input_method.hpp"
#include "background_effect.hpp"
#include "system_bell.hpp"
#include "glass.hpp"
#include "session_management.hpp"
#include "toplevel_icon.hpp"
#include "toplevel_drag.hpp"
#include "paths.hpp"
#include <cstring>
#include <fstream>
#include "registry.hpp"

#include "ipc.hpp"
#include "night_light.hpp"
#include "layer_surface.hpp"
#include "surface_blur.hpp"
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
#ifdef ATRIUM_XWAYLAND
#include "xwayland/xwm.hpp"
#endif

#include <sys/stat.h>
#include <xf86drm.h>

#include <algorithm>
#include <csignal>
#include <cstdlib>
#include <ctime>
#include <sys/wait.h>
#include <unistd.h>

namespace atrium {

namespace {

// Input devices opened through atrium's own session.
class SessionDeviceSeat final : public input::Libinput::DeviceSeat {
public:
    explicit SessionDeviceSeat(backend::Session& s) : s_(s) {}
    struct udev* udev() override { return s_.udev(); }
    const char* name() override { return s_.seat().c_str(); }
    int open(const char* path) override {
        backend::Session::Device* d = s_.open(path);
        return d ? d->fd : (errno ? -errno : -EIO);
    }
    void close(int fd) override { s_.close(s_.device_for_fd(fd)); }

private:
    backend::Session& s_;
};

// ...or through wlroots' (ATRIUM_WLR_DRM=1).
class WlrDeviceSeat final : public input::Libinput::DeviceSeat {
public:
    explicit WlrDeviceSeat(wlr_session* s) : s_(s) {}
    struct udev* udev() override { return s_->udev; }
    const char* name() override { return s_->seat; }
    int open(const char* path) override {
        wlr_device* d = wlr_session_open_file(s_, path);
        if (!d)
            return errno ? -errno : -EIO;
        open_.push_back(d);
        return d->fd;
    }
    void close(int fd) override {
        auto it = std::ranges::find_if(open_, [fd](wlr_device* d) { return d->fd == fd; });
        if (it == open_.end())
            return;
        wlr_session_close_file(s_, *it);
        open_.erase(it);
    }

private:
    wlr_session* s_;
    std::vector<wlr_device*> open_;
};

} // namespace

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
    alog(Log::Error, "%s", what);
    std::exit(EXIT_FAILURE);
}

} // namespace

Server::Server(Config defaults, bool is_nested, std::filesystem::path registry_file)
    : config(defaults), nested(is_nested) {
    g_server = this;
    registry = std::make_unique<Registry>(registry_file.string());
    if (!registry->ok()) {
        alog(Log::Error, "registry: couldn't open %s; nothing will be remembered", registry_file.c_str());
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
        alog(Log::Info, "registry: importing %s", (dir / "settings.json").c_str());

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
    {
        std::vector<std::pair<int64_t, std::string>> words;
        for (const json& s : registry->records(*record_table("snippets")))
            words.emplace_back(s.value("id", int64_t(0)), s.value("keyword", std::string()));
        keywords.set_keywords(std::move(words));
    }
    json rules = json::array();
    auto add = [&](json r, const auto& rec) {
        if (!rec.secret.empty())
            r["secret"] = rec.secret;
        if (rec.space)
            r["space"] = rec.space;
        if (!rec.launch.empty() && !rec.secret.empty())
            r["launch"] = rec.launch;
        if (rec.maximized)
            r["maximized"] = *rec.maximized;
        if (rec.fullscreen)
            r["fullscreen"] = *rec.fullscreen;
        for (auto [key, field] : window_flags(rec))
            if (*field)
                r[key] = **field;
        rules.push_back(std::move(r));
    };
    for (const RuleRecord& r : registry->rules()) {
        json m = json::object();
        if (!r.app_pattern.empty())
            m["app_id"] = r.app_pattern;
        if (!r.title_pattern.empty())
            m["title"] = r.title_pattern;
        add(m, r);
    }
    for (const AppRecord& a : registry->apps()) {
        if (a.secret.empty() && !a.space && !a.maximized && !a.fullscreen &&
            std::ranges::none_of(window_flags(a), [](const auto& f) { return f.second->has_value(); }))
            continue;
        std::string escaped = "^";
        for (char c : a.app_id) {
            if (std::strchr(".^$|()[]{}*+?\\", c))
                escaped += '\\';
            escaped += c;
        }
        add({{"app_id", escaped + "$"}}, a);
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

    // DRM on a TTY, or a window when WAYLAND_DISPLAY/DISPLAY is set. Input
    // on a TTY is atrium's own, from libinput (the seat's): wlroots' libinput
    // backend stays out unless WLR_BACKENDS asks for it.
    const bool nested_session = getenv("WAYLAND_DISPLAY") || getenv("DISPLAY");
    if (!getenv("WLR_BACKENDS") && !nested_session)
        setenv("WLR_BACKENDS", "drm", 1);
    backend_ = std::make_unique<backend::Multi>(loop);
    backend = backend_.get();
    if (const char* b = getenv("WLR_BACKENDS"); b && std::string_view(b) == "headless") {
        // Screens only in memory (tests): atrium's own, as many as
        // WLR_HEADLESS_OUTPUTS says (one by default), as wlroots had them.
        auto h = std::make_unique<backend::Headless>(loop);
        headless_ = h.get();
        const char* n = getenv("WLR_HEADLESS_OUTPUTS");
        const long count = n && *n ? std::strtol(n, nullptr, 10) : 1;
        for (long i = 0; i < count; ++i)
            headless_->add_output(1280, 720);
        backend->add(std::move(h));
    } else if (const char* b = getenv("WLR_BACKENDS");
               getenv("WAYLAND_DISPLAY") && (!b || std::string_view(b) == "wayland")) {
        // Nested in a Wayland session: a window there per screen.
        auto w = backend::Wayland::create(loop);
        if (!w)
            die("couldn't connect to the Wayland session to nest in");
        backend->add(std::move(w));
        // The host session ended: nothing more to show anything on.
        backend_gone_ = backend->events.gone.connect([this] {
            alog(Log::Error, "the backend went away (the host session ended?); quitting");
            if (seat)
                seat->backend_gone();
            wl_display_terminate(display);
        });
    } else if (const char* b = getenv("WLR_BACKENDS");
               !getenv("ATRIUM_WLR_DRM") && (!b || std::string_view(b) == "drm")) {
        // Real screens: atrium's own session and KMS.
        own_session_ = backend::Session::create(loop);
        if (!own_session_)
            die("couldn't take a seat (is logind or seatd running?)");
        // The first GPU that drives renders; screens on the others get copies.
        for (const std::string& gpu : own_session_->find_gpus())
            add_gpu(gpu);
        if (!backend->is_drm())
            die("no GPU with screens to drive");
        gpu_added_ = own_session_->events.add_gpu.connect([this](const std::string& path) { add_gpu(path); });
        device_seat = std::make_unique<SessionDeviceSeat>(*own_session_);
    } else {
        wlroots = wlr_backend_autocreate(loop, &session);
        if (!wlroots)
            die("couldn't create backend");
        // Nested, the backend dies with the session atrium runs inside. wlroots
        // insists nothing still listens on it by then, so let go and end.
        backend_destroy_.connect(&wlroots->events.destroy, [this](void*) {
            alog(Log::Error, "the backend went away (the host session ended?); quitting");
            if (seat)
                seat->backend_gone();
            backend_destroy_.disconnect();
            wlroots = nullptr;
            wl_display_terminate(display);
        });
        backend->add(std::make_unique<backend::WlrBackend>(loop, wlroots));
        if (session)
            device_seat = std::make_unique<WlrDeviceSeat>(session);
    }

    scene = scene::Scene::create();
    root_bg = scene::Rect::create(scene, 0, 0, config.background.data());
    for (auto& tree : layers_)
        tree = scene::Tree::create(scene);
    drag_icons = scene::Tree::create(scene);
    drag_icons->place_below(layer(Layer::Lock));
    snap_preview = std::make_unique<SnapPreview>(*this);
    overview = std::make_unique<Overview>(*this);
    switcher = std::make_unique<Switcher>(*this);
    background_blur = scene::BlurCache::create(scene, 0, 0);
    background_blur->place_above(layer(Layer::Bottom));
    apply_blur_settings();

    // atrium's own GLES 3 renderer: rounded corners, shadows, blur and glass.
    if (render::Renderer* r = render::Renderer::create(*backend))
        renderer = r;
    if (!renderer)
        die("couldn't create renderer");
    gpu_reset_.connect(&renderer->events.lost, [this](void*) { gpu_reset(); });

    apply_screen_shader();

    allocator_ = backend::Allocator::create(renderer->drm_fd());
    allocator = allocator_.get();
    if (!allocator)
        die("couldn't create allocator");

    output_layout_ = std::make_unique<OutputLayout>();
    output_layout = output_layout_.get();
    cursor = std::make_unique<Cursor>(*output_layout, loop);
    scene->draw_cursor = [this](const backend::Output* o, render::RenderPass* pass, const pixman_region32_t* damage) {
        cursor->render(o, pass, damage);
    };
    layout_change_conn_ = output_layout->change.connect([this] { update_outputs(); });
    new_output_conn_ = backend->events.new_output.connect([this](backend::Output* o) { new_output(o); });

    locked_bg = scene::Rect::create(layer(Layer::Lock), 0, 0, config.lock_background.data());
    locked_bg->set_enabled(false);

    setup_protocols();
    setup_window_hints();

    seat = std::make_unique<Seat>(*this);
    // Another VT took the session: libinput lets go of the devices, and
    // takes them back on return.
    if (own_session_)
        session_active_conn_ = own_session_->events.active.connect([this](bool on) { seat->session_active(on); });
    else if (session)
        session_active_.connect(&session->events.active, [this](void*) { seat->session_active(session->active); });
    input_method = std::make_unique<InputMethodRelay>(*this);
    background_effects = std::make_unique<BackgroundEffects>(*this);
    system_bell = std::make_unique<SystemBell>(*this);
    glass_shapes = std::make_unique<GlassShapes>(*this);
    toplevel_drags = std::make_unique<ToplevelDrags>(*this);
    sessions = std::make_unique<SessionManagement>(*this);

    // X clients must never reach the parent X server when running nested.
    unsetenv("DISPLAY");
#ifdef ATRIUM_XWAYLAND
    // Not lazy: an app elevated with pkexec may be the first X client, and
    // it must find root already let in (allow_root_x11), which only works
    // once Xwayland is up. A lazily started one also forgets it on restart.
    wlr_xwayland_server_options xopts{};
    xopts.enable_wm = true;
    xwayland = wlr_xwayland_server_create(display, &xopts);
    if (xwayland) {
        // Only Xwayland may bind xwayland_shell_v1.
        xwayland_start_.connect(&xwayland->events.start,
                                [this](void*) { wl->xwayland_shell->set_client(xwayland->client); });
        xwayland_ready_.connect(&xwayland->events.ready, [this](wlr_xwayland_server_ready_event* e) {
            xwm = std::make_unique<xwayland::Xwm>(display, e->wm_fd, xwayland->client, *wl->compositor,
                                                  wl->xwayland_shell.get(), false);
            if (!xwm->ok()) {
                xwm.reset();
                return;
            }
            new_x11_window_ = xwm->events.new_surface.connect(
                [this](xwayland::XSurface* s) { new XwaylandView(*this, s); });
            // Xwayland gone (it restarts on the next X client): so is its WM.
            xwm_hangup_ = xwm->events.hangup.connect([this] {
                new_x11_window_.disconnect();
                xwm_hangup_.disconnect();
                wl_event_loop_add_idle(loop, [](void* data) { static_cast<Server*>(data)->xwm.reset(); }, this);
            });
            xwm->set_seat(wl->seat.get(), wl->data.get(), wl->primary.get());
            seat->set_x11_cursor();
            allow_root_x11(xwayland->display_name);
            run_startup();
        });
        setenv("DISPLAY", xwayland->display_name, 1);
    } else {
        alog(Log::Error, "failed to set up Xwayland, continuing without it");
    }
#endif
}

// What a client should allocate for a surface: for scan-out on `scanout`
// first (when it could go straight there), then for rendering.
static wl::DmabufFeedback dmabuf_feedback(render::Renderer* renderer, backend::Output* scanout) {
    wl::DmabufFeedback fb;
    struct stat st{};
    if (int fd = renderer->drm_fd(); fd >= 0 && fstat(fd, &st) == 0)
        fb.main_device = st.st_rdev;
    const wlr_drm_format_set* texture = renderer->texture_formats(BUFFER_CAP_DMABUF);
    auto has = [](const wlr_drm_format_set* set, uint32_t format, uint64_t modifier) {
        return set && wlr_drm_format_set_has(set, format, modifier);
    };
    if (scanout) {
        if (const wlr_drm_format_set* primary = scanout->primary_formats(BUFFER_CAP_DMABUF)) {
            wl::DmabufFeedback::Tranche t;
            t.target_device = fb.main_device;
            t.scanout = true;
            for (size_t i = 0; i < primary->len; ++i)
                for (size_t j = 0; j < primary->formats[i].len; ++j)
                    if (has(texture, primary->formats[i].format, primary->formats[i].modifiers[j]))
                        t.formats.emplace_back(primary->formats[i].format, primary->formats[i].modifiers[j]);
            if (!t.formats.empty())
                fb.tranches.push_back(std::move(t));
        }
    }
    wl::DmabufFeedback::Tranche render;
    render.target_device = fb.main_device;
    if (texture)
        for (size_t i = 0; i < texture->len; ++i)
            for (size_t j = 0; j < texture->formats[i].len; ++j)
                render.formats.emplace_back(texture->formats[i].format, texture->formats[i].modifiers[j]);
    fb.tranches.push_back(std::move(render));
    return fb;
}

void Server::setup_protocols() {
    wl = std::make_unique<Protocols>();
    Protocols& p = *wl;
    auto& c = connections_;

    // Buffers and surfaces.
    std::vector<uint32_t> shm_formats;
    if (const wlr_drm_format_set* f = renderer->texture_formats(BUFFER_CAP_DATA_PTR))
        for (size_t i = 0; i < f->len; ++i)
            shm_formats.push_back(f->formats[i].format);
    p.shm = std::make_unique<wl::Shm>(display, shm_formats);
    if (renderer->texture_formats(BUFFER_CAP_DMABUF)) {
        // A dmabuf the renderer can't import is refused as it is made.
        auto check = [this](const DmabufAttributes& attrs) {
            render::Texture* t = renderer->texture_from_dmabuf(const_cast<DmabufAttributes*>(&attrs));
            if (t)
                t->destroy();
            return t != nullptr;
        };
        p.dmabuf = std::make_unique<wl::LinuxDmabuf>(display, dmabuf_feedback(renderer, nullptr), check);
        std::vector<uint32_t> formats;
        const wlr_drm_format_set* f = renderer->texture_formats(BUFFER_CAP_DMABUF);
        for (size_t i = 0; i < f->len; ++i)
            formats.push_back(f->formats[i].format);
        if (char* node = drmGetRenderDeviceNameFromFd(renderer->drm_fd())) {
            p.drm = std::make_unique<wl::LegacyDrm>(display, node, formats, check);
            free(node);
        }
    }
    if (int drm_fd = renderer->drm_fd();
        drm_fd >= 0 && renderer->features.timeline && backend->supports_timelines())
        p.syncobj = std::make_unique<wl::Syncobj>(display, drm_fd);
    p.compositor = std::make_unique<wl::Compositor>(display, renderer);
    p.viewporter = std::make_unique<wl::Viewporter>(display);
    p.single_pixel = std::make_unique<wl::SinglePixelBuffers>(display);
    p.fractional_scales = std::make_unique<wl::FractionalScales>(display);
    p.surface_hints = std::make_unique<wl::SurfaceHints>(display);
    p.presentation = std::make_unique<wl::Presentation>(display);
    p.fifo = std::make_unique<wl::Fifo>(display);
    p.commit_timing = std::make_unique<wl::CommitTiming>(display);
    // Color management: apps say what their content is (an HDR video, a
    // game's HDR10 swapchain) and hear what a screen prefers. The renderer
    // converts PQ or linear content in BT.2020 or sRGB primaries.
    p.color = std::make_unique<wl::ColorManagement>(display, wl::ColorManagement::Options{
        .intents = {0},          // perceptual
        .features = {1, 5},      // parametric, mastering display primaries
        .transfer_functions = {2, 11, 5},  // gamma 2.2, PQ, linear
        .primaries = {1, 6},     // sRGB, BT.2020
    });

    scene->protocols = {
        .dmabuf = p.dmabuf.get(),
        .dmabuf_feedback = [this](backend::Output* scanout) { return dmabuf_feedback(renderer, scanout); },
        .fractional_scales = p.fractional_scales.get(),
        .color = p.color.get(),
        .syncobj = p.syncobj.get(),
    };

    // Input.
    p.seat = std::make_unique<wl::Seat>(display, "seat0");
    p.data = std::make_unique<wl::DataDevices>(display, *p.seat);
    p.primary = std::make_unique<wl::PrimarySelection>(display, *p.seat);
    p.data_control = std::make_unique<wl::DataControl>(display, *p.seat, p.data->slot(), p.primary->slot());
    p.relative_pointers = std::make_unique<wl::RelativePointers>(display, *p.seat);
    p.pointer_constraints = std::make_unique<wl::PointerConstraints>(display, *p.seat);
    p.pointer_gestures = std::make_unique<wl::PointerGestures>(display, *p.seat);
    p.pointer_warps = std::make_unique<wl::PointerWarps>(display, *p.seat);
    // A client that asks (a VM, remote desktop, the Settings app recording
    // a shortcut) gets the keys atrium would otherwise take, while focused.
    p.shortcut_inhibitors = std::make_unique<wl::ShortcutInhibitors>(display);
    c.push_back(p.shortcut_inhibitors->new_inhibitor.connect(
        [&p](wl::ShortcutInhibitors::Inhibitor* i) { p.shortcut_inhibitors->set_active(i, true); }));
    p.cursor_shapes = std::make_unique<wl::CursorShapes>(display, *p.seat);
    p.idle_inhibitors = std::make_unique<wl::IdleInhibitors>(display);
    c.push_back(p.idle_inhibitors->changed.connect([this] { check_idle_inhibitors(); }));
    p.idle_notifier = std::make_unique<wl::IdleNotifier>(display, *p.seat);
    p.text_inputs = std::make_unique<wl::TextInputs>(display, *p.seat);
    p.input_methods = std::make_unique<wl::InputMethods>(display, *p.seat);
    p.virtual_inputs = std::make_unique<wl::VirtualInputs>(display, *p.seat);
    p.tablets = std::make_unique<wl::Tablets>(display, *p.seat);

    // Windows.
    p.xdg = std::make_unique<wl::Shell>(display);
    c.push_back(p.xdg->events.new_toplevel.connect([this](wl::Toplevel* t) { new XdgView(*this, t); }));
    c.push_back(p.xdg->events.new_popup.connect([this](wl::Popup* popup) { handle_new_xdg_popup(*this, popup); }));
    p.pip = std::make_unique<wl::PipShell>(display);
    // atrium decorates every window that lets it, over both protocols.
    p.decorations = std::make_unique<wl::Decorations>(display, wl::Decorations::ServerSide);
    c.push_back(p.decorations->events.new_xdg.connect([](wl::Decorations::Xdg* d) {
        if (auto* view = static_cast<XdgView*>(d->toplevel->data))
            view->set_decoration(d);
    }));
    // Usually arrives before the surface has its toplevel role; XdgView
    // picks those up itself when it is created.
    c.push_back(p.decorations->events.new_kde.connect([](wl::Decorations::Kde* d) {
        if (wl::Toplevel* t = wl::Toplevel::from(d->surface); t && t->data)
            static_cast<XdgView*>(t->data)->set_kde_decoration(d);
    }));
    p.dialogs = std::make_unique<wl::Dialogs>(display);
    p.activation = std::make_unique<wl::Activation>(display, *p.seat);
    c.push_back(p.activation->request_activate.connect([this](const wl::Activation::Request& r) {
        View* view = owner_of(r.surface).view;
        if (!view || view == focused_view)
            return;
        const bool from_input = r.token && r.token->seat;
        // Asked for before it has shown (a terminal launched from a shortcut
        // does): it takes focus as it opens.
        if (!view->mapped) {
            view->activate_on_map = from_input;
            return;
        }
        // A token minted from real user input (a click on a link, a
        // notification) may take focus; anything else only marks the
        // window as wanting attention.
        if (from_input) {
            if (view->minimized)
                view->set_minimized(false);
            focus_view(view);
        } else {
            view->urgent = true;
        }
    }));
    p.tags = std::make_unique<wl::ToplevelTags>(display);
    p.foreign = std::make_unique<wl::XdgForeign>(display);
    p.layer_shell = std::make_unique<wl::LayerShell>(display);
    c.push_back(p.layer_shell->new_surface.connect([this](wl::LayerSurface* l) {
        if (!l->output()) {
            if (!focused_output) {
                l->close();
                return;
            }
            l->set_output(focused_output->global.get());
        }
        new LayerSurface(*this, l);
    }));
    p.session_lock = std::make_unique<wl::SessionLockManager>(display);
    c.push_back(p.session_lock->new_lock.connect([this](wl::Lock* l) {
        locked_bg->set_enabled(true);
        if (lock) {  // one lock at a time
            l->finish();
            return;
        }
        lock = new SessionLock(*this, l);
    }));
    p.xwayland_shell = std::make_unique<wl::XwaylandShell>(display);

    // The desktop.
    p.xdg_outputs = std::make_unique<wl::XdgOutputs>(display);
    p.output_management = std::make_unique<wl::OutputManagement>(display);
    p.output_power = std::make_unique<wl::OutputPower>(display);
    p.gamma = std::make_unique<wl::GammaControls>(display);
    scene->set_gamma_controls(p.gamma.get());
    p.toplevels = std::make_unique<wl::ForeignToplevels>(display);
    p.workspaces = std::make_unique<wl::Workspaces>(display);
    c.push_back(p.workspaces->requests.connect(
        [this](const std::vector<wl::Workspaces::Request>& r) { workspace_requests(r); }));
    p.capture = std::make_unique<wl::Capture>(display, *p.seat, *p.toplevels);
    p.global_shortcuts = std::make_unique<wl::GlobalShortcuts>(display);
    p.security = std::make_unique<wl::SecurityContexts>(display);
    setup_outputs_protocols();
    setup_capture();
}

// Every listener must be off its signal before the object carrying the signal
// is freed: a Listener destroyed later would unlink from freed memory.
void Server::disconnect_listeners() {
    new_output_conn_.disconnect();
    layout_change_conn_.disconnect();
    gpu_reset_.disconnect();
    session_active_.disconnect();
    connections_.clear();
#ifdef ATRIUM_XWAYLAND
    xwayland_start_.disconnect();
    xwayland_ready_.disconnect();
    new_x11_window_.disconnect();
    xwm_hangup_.disconnect();
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
        alog(Log::Error, "xwayland: couldn't connect to %s to allow root", display);
        xcb_disconnect(conn);
        return;
    }
    static constexpr char kRoot[] = "localuser\0root";
    xcb_generic_error_t* err = xcb_request_check(conn,
        xcb_change_hosts_checked(conn, XCB_HOST_MODE_INSERT, XCB_FAMILY_SERVER_INTERPRETED,
                                 sizeof kRoot - 1, reinterpret_cast<const uint8_t*>(kRoot)));
    if (err) {
        alog(Log::Error, "xwayland: allowing root failed (X error %d)", err->error_code);
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
            alog(Log::Info, "clipboard history: %s not installed", tool);
            return;
        }
    spawn("exec wl-paste --type text --watch cliphist store");
    spawn("exec wl-paste --type image --watch cliphist store");
}

// atrium-clipsync, the clipboard shared with a phone: always started, it
// idles while bluetooth.phone_clipboard is off. BlueZ is the machine's, so
// a nested atrium leaves it to the host session.
void Server::start_clipboard_sync() {
    namespace fs = std::filesystem;
    if (nested && !std::getenv("ATRIUM_CLIPSYNC"))
        return;
    std::error_code ec;
    const fs::path exe = fs::read_symlink("/proc/self/exe", ec);
    fs::path bin = fs::path(ATRIUM_BINDIR) / "atrium-clipsync";
    if (const fs::path built = fs::path(ATRIUM_BUILD_DIR) / "shell" / "clipsync" / "atrium-clipsync";
        !ec && exe.string().starts_with(ATRIUM_BUILD_DIR) && fs::exists(built))
        bin = built;
    if (fs::exists(bin))
        spawn("exec '" + bin.string() + "'");
}

// Things the hardware forgets between boots (the monitors' brightness is the
// shell's: it talks to them anyway, and two talking at once collide).
void Server::restore_power_and_brightness() {
    // Only on a real screen: not nested, not headless (a test).
    if (nested || !backend->is_drm())
        return;
    apply_power_profile();
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
    night_light.reset();
    ipc.reset();
    disconnect_listeners();
#ifdef ATRIUM_XWAYLAND
    xwm.reset();
    if (xwayland)
        wlr_xwayland_server_destroy(xwayland);
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
    system_bell.reset();
    glass_shapes.reset();
    toplevel_drags.reset();
    sessions.reset();
    toplevel_icons.reset();
    seat.reset();
    capture_.reset();

    // The backend by hand before the display: its outputs go (and tell the
    // protocols so), then the globals, before the display.
    backend_destroy_.disconnect();
    backend_gone_.disconnect();
    new_output_conn_.disconnect();
    backend = nullptr;
    headless_ = nullptr;
    session_active_conn_.disconnect();
    gpu_added_.disconnect();
    gpu_removed_.clear();
    gpu_leases_.clear();  // before the GPUs they lease from
    backend_.reset();  // takes wlroots' with it, unless the host session ended
    device_seat.reset();
    own_session_.reset();
    wlroots = nullptr;
    layout_change_conn_.disconnect();
    scene->draw_cursor = nullptr;
    cursor.reset();
    output_layout = nullptr;
    output_layout_.reset();
    scene->protocols = {};
    scene->set_gamma_controls(nullptr);
    wl.reset();
    wl_display_destroy(display);
    allocator = nullptr;
    allocator_.reset();
    // Only after the display: outputs are gone and no scene output is left.
    (scene)->destroy();
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
    // sudo -A and ssh ask through the shell, unless the user chose otherwise.
    if (const std::string askpass = ATRIUM_BINDIR "/atrium-askpass"; access(askpass.c_str(), X_OK) == 0) {
        setenv("SUDO_ASKPASS", askpass.c_str(), 0);
        setenv("SSH_ASKPASS", askpass.c_str(), 0);
    }
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
        // The login password opens the KDE wallet (Chrome's and Spotify's
        // keys): pam_kwallet left a socket for it in our environment only,
        // and Plasma's own service for it (or the autostart entry, which
        // systemd skips) can't see it. Before anything asks for a secret.
        if (std::getenv("PAM_KWALLET5_LOGIN") && access("/usr/lib/pam_kwallet_init", X_OK) == 0)
            spawn("/usr/lib/pam_kwallet_init");
        spawn("dbus-update-activation-environment --systemd WAYLAND_DISPLAY XDG_CURRENT_DESKTOP "
              "XDG_SESSION_TYPE XDG_MENU_PREFIX DISPLAY GTK_THEME QT_QPA_PLATFORMTHEME QTENGINE_CONFIG XDG_CONFIG_DIRS "
              "SUDO_ASKPASS SSH_ASKPASS; "
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
    if (!config.greeter)
        night_light = std::make_unique<NightLight>(*this);

    if (!backend->start())
        die("couldn't start backend");

    shell = std::make_unique<ShellProcess>(*this);
    shell->start();
    if (!config.greeter) {
        start_clipboard_history();
        start_clipboard_sync();
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

    focused_output = output_at(cursor->x, cursor->y);
    // Put the cursor image where the cursor actually is.
    cursor->warp_closest(cursor->x, cursor->y);
    seat->set_default_cursor();

    alog(Log::Info, "running on WAYLAND_DISPLAY=%s", socket);
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

// A GPU's leasing global, and what it asks of the GPU.
struct Server::GpuLease final : wl::DrmLease::Provider {
    backend::drm::Drm* drm;
    std::unique_ptr<wl::DrmLease> lease;
    wl::Connection ended;
    GpuLease(wl_display* display, backend::drm::Drm* d) : drm(d) {
        lease = std::make_unique<wl::DrmLease>(display, *this);
        ended = drm->lease_ended.connect([this](uint32_t lessee) { lease->lease_ended(lessee); });
    }
    int non_master_fd() override { return drm->non_master_fd(); }
    int create_lease(const std::vector<uint32_t>& c, uint32_t* lessee) override { return drm->create_lease(c, lessee); }
    void revoke_lease(uint32_t lessee) override { drm->revoke_lease(lessee); }
};

Server::GpuLease* Server::lease_for(const backend::Backend* gpu) const {
    for (const auto& g : gpu_leases_)
        if (g->drm == gpu)
            return g.get();
    return nullptr;
}

// A GPU's screens (at startup, or an eGPU or dock plugged in later). Unplugged,
// a secondary one goes with its screens; the primary renders everything.
void Server::add_gpu(const std::string& path) {
    auto d = backend::drm::Drm::create(loop, *own_session_, path, primary_gpu_);
    if (!d)
        return;
    backend::drm::Drm* raw = d.get();
    if (!primary_gpu_)
        primary_gpu_ = raw;
    else
        gpu_removed_.push_back(raw->removed.connect([this, raw] {
            // Not from inside the session's own device list: on the next idle.
            pending_gpu_removal_.push_back(raw);
            wl_event_loop_add_idle(loop, [](void* data) {
                auto* self = static_cast<Server*>(data);
                for (backend::drm::Drm* g : std::exchange(self->pending_gpu_removal_, {})) {
                    alog(Log::Info, "drm: %s unplugged", g->name().c_str());
                    std::erase_if(self->gpu_leases_, [g](const auto& l) { return l->drm == g; });
                    self->backend->remove(g);
                }
            }, this);
        }));
    gpu_leases_.push_back(std::make_unique<GpuLease>(display, raw));
    backend->add(std::move(d));
}

void Server::quit() {
    wl_display_terminate(display);
}

// --- outputs -----------------------------------------------------------------

void Server::new_output(backend::Output* wlr) {
    // A VR headset: not part of the desktop, offered to clients to drive.
    if (wlr->non_desktop) {
        if (GpuLease* g = lease_for(&wlr->backend)) {
            const uint32_t id = g->drm->connector_id(wlr);
            alog(Log::Info, "%s is not a desktop screen: offered for lease", wlr->name.c_str());
            g->lease->offer(id, wlr->name, wlr->description);
            auto conn = std::make_shared<wl::Connection>();
            *conn = wlr->events.destroy.connect([this, gpu = &wlr->backend, id, conn] {
                if (GpuLease* l = lease_for(gpu))
                    l->lease->withdraw(id);
                conn->disconnect();
            });
        }
        return;
    }
    if (!wlr->init_render(allocator, renderer))
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
    backend::Output* o = output_layout->output_at(lx, ly);
    return o ? static_cast<Output*>(o->data) : nullptr;
}

// Called whenever the layout changes: an output appears, disappears, changes
// mode or position. Recomputes every box that depends on outputs and publishes
// the new state to wlr-output-management clients.
void Server::update_outputs() {
    if (night_light)
        night_light->update();  // a screen that can (or can't) show it came or went
    // Disabled outputs leave the layout first, so the cursor cannot enter them.
    for (Output* o : outputs) {
        if (o->enabled() || o->dying)  // leaving: already out of the layout
            continue;
        if (o->asleep)
            continue;
        output_layout->remove(o->screen);
        o->box = o->usable = {};
    }
    for (Output* o : outputs) {
        if (o->enabled() && !o->dying && !output_layout->contains(o->screen))
            output_layout->add_auto(o->screen);
    }

    layout_box = output_layout->extents();
    background_blur->set_position(layout_box.x, layout_box.y);
    background_blur->set_size(layout_box.width, layout_box.height);
    background_blur->mark_dirty();
    root_bg->set_position(layout_box.x, layout_box.y);
    root_bg->set_size(layout_box.width, layout_box.height);
    locked_bg->set_position(layout_box.x, layout_box.y);
    locked_bg->set_size(layout_box.width, layout_box.height);

    for (Output* o : outputs) {
        if (!o->enabled() || o->dying)
            continue;
        o->box = output_layout->box(o->screen);
        o->usable = o->box;
        if (o->scene_output)
            o->scene_output->set_position(o->box.x, o->box.y);
        o->sync_global();
        o->fullscreen_bg->set_position(o->box.x, o->box.y);
        o->fullscreen_bg->set_size(o->box.width, o->box.height);

        if (o->lock_surface) {
            auto* tree = static_cast<scene::Tree*>(o->lock_surface->surface()->data);
            tree->set_position(o->box.x, o->box.y);
            o->lock_surface->configure(uint32_t(o->box.width), uint32_t(o->box.height));
        }

        o->arrange_layers();
        o->refit_views();

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
            seat->keyboard_enter(focused_output->lock_surface->surface());
    }

    // The cursor image can end up at 0,0 after outputs come back; re-place it.
    cursor->move(0, 0);

    publish_outputs();
    if (ipc)
        ipc->broadcast("outputs", {{"event", "outputs.changed"}});
}

void Server::gpu_reset() {
    render::Renderer* old_renderer = renderer;
    std::unique_ptr<backend::Allocator> old_allocator = std::move(allocator_);

    render::Renderer* r = render::Renderer::create(*backend);
    renderer = r ? r : nullptr;
    if (!renderer)
        die("couldn't recreate renderer");
    allocator_ = backend::Allocator::create(renderer->drm_fd());
    allocator = allocator_.get();
    if (!allocator)
        die("couldn't recreate allocator");

    gpu_reset_.connect(&renderer->events.lost, [this](void*) { gpu_reset(); });
    wl->compositor->set_renderer(renderer);
    apply_screen_shader();
    for (Output* o : outputs)
        o->screen->init_render(allocator, renderer);
    cursor->reset_render();  // its textures and plane buffers were the old ones'

    old_allocator.reset();
    old_renderer->destroy();
}

// --- focus and hit testing -------------------------------------------------

Owner Server::owner_of(wl::Surface* surface) {
    Owner owner;
    if (!surface)
        return owner;
    wl::Surface* root = surface->root();
#ifdef ATRIUM_XWAYLAND
    if (auto* xs = xwayland::XSurface::from(root)) {
        owner.view = static_cast<View*>(xs->data);
        return owner;
    }
#endif
    if (auto* ls = wl::LayerSurface::from(root)) {
        owner.layer = static_cast<LayerSurface*>(ls->data);
        return owner;
    }
    // Popups up to the toplevel or layer surface they belong to.
    for (wl::ShellSurface* xdg = wl::ShellSurface::from(root); xdg;) {
        switch (xdg->kind()) {
        case wl::ShellSurface::Kind::Popup: {
            wl::Popup* p = xdg->popup();
            if (!p || !p->parent())
                return owner;
            wl::ShellSurface* parent = wl::ShellSurface::from(p->parent());
            if (!parent)
                return owner_of(p->parent());
            xdg = parent;
            break;
        }
        case wl::ShellSurface::Kind::Toplevel:
            owner.view = static_cast<View*>(xdg->toplevel()->data);
            return owner;
        default:
            return owner;
        }
    }
    return owner;
}

Hit Server::hit_test(double lx, double ly, View* through) const {
    // Out of the scene for the lookup only; nothing draws in between.
    const bool hide = through && through->tree && through->tree->enabled;
    if (hide)
        through->tree->set_enabled(false);
    Hit hit = hit_test_scene(lx, ly);
    if (hide)
        through->tree->set_enabled(true);
    return hit;
}

Hit Server::hit_test_scene(double lx, double ly) const {
    Hit hit;
    const int lowest = locked ? int(Layer::Lock) : 0;
    for (int l = kLayerCount - 1; l >= lowest && !hit.surface; --l) {
        // A secret space still fading away takes no pointer.
        if (l == int(Layer::InputPopup) || (l == int(Layer::Secret) && !shown_secret))
            continue;
        scene::Node* node = layers_[l]->at(lx, ly, &hit.sx, &hit.sy);
        if (!node || (node->type != scene::Type::Buffer && node->type != scene::Type::Rect))
            continue;
        if (node->type == scene::Type::Rect) {
            // Only a secret space's backdrop is a rect that carries data.
            if (node->data) {
                auto* space = static_cast<Space*>(node->data);
                if (!space->shown())
                    continue;
                hit.backdrop = space;
                return hit;
            }
            continue;
        }
        if (auto* ss = (static_cast<scene::Buffer*>(node))->surface()) {
            hit.surface = ss->surface;
        } else if (node->data) {
            // The only non-surface buffers carrying data are title bars.
            hit.titlebar = static_cast<Titlebar*>(node->data);
            hit.view = &hit.titlebar->view();
            break;
        }
    }
    if (!hit.titlebar) {
        Owner owner = owner_of(hit.surface);
        hit.view = owner.view;
        hit.layer = owner.layer;
    }
    // A space sliding or fading out is still drawn but already gone: its
    // windows take no pointer (focusing one would bring the space back).
    if (hit.view && hit.view->space && !hit.view->space->shown())
        return Hit{};
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
    const Box b = away ? view->restore : view->geom;
    const Box o = view->output ? view->output->box : Box{};
    return Placement{view->output ? view->output->screen->name : "", b.x - o.x, b.y - o.y, b.width, b.height,
                     view->maximized, view->snapped};
}

void Server::remember_placement(const View* view) {
    if (sessions)
        sessions->view_unmapping(view);
    // A secret space's size belongs to the space, not the app; a tile's to
    // the layout.
    if (!config.remember_placement || !placeable(view) || !view->output || !view->mapped ||
        (view->space && view->space->secret) || view->tiled())
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
    if (locked || (view && !view->mapped))
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

    // Most recently used first, even when it has the keys already.
    if (view && !view->unmanaged()) {
        std::erase(views, view);
        views.insert(views.begin(), view);
        if (view->output)
            focused_output = view->output;
        view->urgent = false;
    }
    restack_fullscreen();

    wl::Surface* old = wl->seat->keyboard_focus();
    if (view && view->surface() == old)
        return;

    Owner old_owner = owner_of(old);
    if (old_owner.view && old_owner.view->kind == View::Kind::Xdg)
        static_cast<XdgView*>(old_owner.view)->dismiss_popups();

    // A top/overlay layer surface holding exclusive keyboard focus (a lock
    // screen stand-in, a launcher) keeps it; the view only moves up the order.
    if (old_owner.layer && old_owner.layer->mapped && old_owner.layer->wants_exclusive_keyboard() &&
        old_owner.layer->ls->current().layer >= ZWLR_LAYER_SHELL_V1_LAYER_TOP)
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

std::optional<Box> Server::dock_icon_of(const View& view) const {
    if (!view.output)
        return std::nullopt;
    auto it = dock_icons.find(view.output->screen->name);
    if (it == dock_icons.end())
        return std::nullopt;
    // The Dock's own surface, where its icons were measured.
    const LayerSurface* dock = nullptr;
    for (const auto& list : view.output->layers)
        for (const LayerSurface* l : list)
            if (l->mapped && l->ls->name_space() == "atrium-dock")
                dock = l;
    if (!dock || !dock->tree)
        return std::nullopt;
    int lx = 0, ly = 0;
    dock->tree->coords(&lx, &ly);
    for (const DockIcon& icon : it->second)
        if (std::ranges::find(icon.windows, view.id) != icon.windows.end())
            return Box{lx + icon.box.x, ly + icon.box.y, icon.box.width, icon.box.height};
    return std::nullopt;
}

// Fullscreen windows over the panels only while in front (stacking::in_front_of).
void Server::restack_fullscreen() {
    std::vector<stacking::Window> mru;
    for (const View* v : views)
        mru.push_back({v->fullscreen, v->mapped && v->tree && !v->minimized, !v->unmanaged(), v->output, v->space});
    for (size_t i = 0; i < views.size(); ++i) {
        View* f = views[i];
        if (!f->fullscreen || !f->tree || f->minimized || f->unmanaged())
            continue;
        const int j = stacking::in_front_of(mru, i);
        View* front = j >= 0 ? views[size_t(j)] : nullptr;
        if ((front != nullptr) == f->covered)
            continue;
        f->covered = front != nullptr;
        f->tree->reparent(f->home_tree());
        if (front && front->tree->parent == f->tree->parent)
            f->tree->place_below(front->tree);
        f->update_decorations();
        if (f->output)
            f->output->refit_views();  // the backdrop behind it
        notify_window(*f, "changed");
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
    // The shell's menus and panels above windows take the keyboard but, as
    // on a Mac, the window stays the active one under them (its title bar,
    // the bar's app name). The desktop below windows is another matter: it
    // takes focus the way an app does.
    if (focused_view && layer->ls->current().layer < ZWLR_LAYER_SHELL_V1_LAYER_TOP)
        drop_focus();
    seat->keyboard_enter(layer->surface());
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

// --- idle ----------------------------------------------------------------------

void Server::check_idle_inhibitors() {
    bool inhibited = false;
    for (wl::Surface* surface : wl->idle_inhibitors->surfaces()) {
        auto* tree = static_cast<scene::Tree*>(surface->root()->data);
        int x, y;
        if (config.idle_inhibit_ignore_visibility || !tree || tree->coords(&x, &y)) {
            inhibited = true;
            break;
        }
    }
    wl->idle_notifier->set_inhibited(inhibited);
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
        alog_errno(Log::Error, "fork failed for '%s'", command.c_str());
    }
}

void Server::change_vt(unsigned vt) {
    if (own_session_)
        own_session_->change_vt(vt);
    else if (session)
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
    case Action::SnapLeft: if (v) v->snap(EDGE_LEFT); break;
    case Action::SnapRight: if (v) v->snap(EDGE_RIGHT); break;
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
    case Action::Place:
        if (v)
            place_window(v, b.arg);
        break;
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
    if (is("appearance.blur") || is("appearance.glass") || key == "appearance.liquid_glass")
        apply_blur_settings();
    if (key == "appearance.screen_shader")
        apply_screen_shader();
    if (is("appearance.blur") || key == "appearance.transparency")
        refresh_surface_blurs();
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
        root_bg->set_color(config.background.data());
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
    if (key.starts_with("displays.night_light") && night_light) {
        if (key == "displays.night_light_warmth")
            night_light->update();
        else
            night_light->schedule_changed();
    }
    if (key == "session.shell" && shell)
        shell->restart();
    if (key == "windows.fullscreen_space" && !config.fullscreen_space)
        for (View* v : views)
            v->fullscreen_home = 0;
    if (key == "windows.tiled_titlebars") {
        for (View* v : views)
            v->refresh_tiled_titlebar();
        for (auto& space : spaces)
            retile(space.get());
    }
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

// The user's shader over every screen; one that doesn't compile leaves
// the screens as they are (the log says why).
void Server::apply_screen_shader() {
    render::Renderer* r = renderer;
    if (!r)
        return;
    r->egl().make_current();
    r->shaders().set_screen_shader(config.screen_shader);
    for (Output* o : outputs)
        if (o->scene_output)
            o->scene_output->damage_whole();
}

void Server::apply_blur_settings() {
    // Liquid Glass lets the colours behind through, brighter and richer;
    // frosted glass dims and greys them a little.
    render::BlurParams p = config.liquid_glass
        ? render::BlurParams{config.blur_passes, float(config.blur_radius), 0.01f, 1.02f, 0.95f, 1.45f}
        : render::BlurParams{config.blur_passes, float(config.blur_radius), 0.02f, 0.9f, 0.9f, 1.1f};
    // The settings adjust the style's own look.
    p.noise *= config.blur_noise;
    p.brightness *= config.blur_brightness;
    p.contrast *= config.blur_contrast;
    p.saturation *= config.blur_saturation;
    scene->set_blur(p);
    background_blur->set_enabled(config.blur);
    for (View* v : views)
        v->update_decorations();
    for (Output* o : outputs)
        for (auto& list : o->layers)
            for (LayerSurface* l : list)
                l->refresh_blur();
    background_blur->mark_dirty();
}

void Server::show_window_menu(const View* view, double lx, double ly) {
    Output* o = output_at(lx, ly);
    if (!ipc || !view || view->unmanaged() || !o)
        return;
    ipc->broadcast("windows", {{"event", "window.menu"},
                               {"window", Ipc::window_json(*view)},
                               {"output", o->screen->name},
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
