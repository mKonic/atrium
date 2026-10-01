// Desktop-integration protocols (src/wl: desktop, misc) against a real
// libwayland client.
#include "wl/compositor.hpp"
#include "wl/desktop.hpp"
#include "wl/misc.hpp"
#include "wl/output.hpp"
#include "wl/seat.hpp"
#include "wl/xdg_shell.hpp"
#include "wl_harness.hpp"

#include "ext-foreign-toplevel-list-v1-client-protocol.h"
#include "ext-workspace-v1-client-protocol.h"
#include "hyprland-global-shortcuts-v1-client-protocol.h"
#include "security-context-v1-client-protocol.h"
#include "wlr-foreign-toplevel-management-unstable-v1-client-protocol.h"
#include "wlr-gamma-control-unstable-v1-client-protocol.h"
#include "wlr-output-management-unstable-v1-client-protocol.h"
#include "xdg-foreign-unstable-v2-client-protocol.h"
#include "xdg-shell-client-protocol.h"

#include <wayland-client-protocol.h>

#include <gtest/gtest.h>

#include <sys/mman.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

using namespace atrium;

namespace {

struct Desk : wltest::Harness {
    wl::Compositor compositor{server, nullptr};
    wl::Seat seat{server, "seat0"};
    wl::Output output{server, wl::OutputInfo{.name = "DP-1"}};
    wl_output* wout = nullptr;
    wl_seat* wseat = nullptr;
    Desk() {
        wout = bind<wl_output>(&wl_output_interface, 4);
        wseat = bind<wl_seat>(&wl_seat_interface);
        pump();
    }
    ~Desk() {
        if (!client)
            return;
        wl_seat_release(wseat);
        wl_output_release(wout);
        pump();
    }
};

} // namespace

TEST(WlDesktop, ForeignToplevelsInBothProtocols) {
    Desk d;
    wl::ForeignToplevels ft(d.server);
    wl::ForeignToplevels::Handle* h = ft.create({.title = "Mail", .app_id = "org.mail", .outputs = {&d.output}});

    struct Log {
        std::string ext_title, wlr_title;
        int wlr_done = 0, closed = 0, entered = 0;
        std::vector<uint32_t> states;
        zwlr_foreign_toplevel_handle_v1* wlr = nullptr;
        ext_foreign_toplevel_handle_v1* ext = nullptr;
    } log;
    static const ext_foreign_toplevel_handle_v1_listener eh = {
        .closed = [](void* p, ext_foreign_toplevel_handle_v1*) { ++static_cast<Log*>(p)->closed; },
        .done = [](void*, ext_foreign_toplevel_handle_v1*) {},
        .title = [](void* p, ext_foreign_toplevel_handle_v1*,
                    const char* t) { static_cast<Log*>(p)->ext_title = t; },
        .app_id = [](void*, ext_foreign_toplevel_handle_v1*, const char*) {},
        .identifier = [](void*, ext_foreign_toplevel_handle_v1*, const char*) {},
    };
    static const ext_foreign_toplevel_list_v1_listener el = {
        .toplevel =
            [](void* p, ext_foreign_toplevel_list_v1*, ext_foreign_toplevel_handle_v1* h) {
                static_cast<Log*>(p)->ext = h;
                ext_foreign_toplevel_handle_v1_add_listener(h, &eh, p);
            },
        .finished = [](void*, ext_foreign_toplevel_list_v1*) {},
    };
    static const zwlr_foreign_toplevel_handle_v1_listener wh = {
        .title = [](void* p, zwlr_foreign_toplevel_handle_v1*,
                    const char* t) { static_cast<Log*>(p)->wlr_title = t; },
        .app_id = [](void*, zwlr_foreign_toplevel_handle_v1*, const char*) {},
        .output_enter = [](void* p, zwlr_foreign_toplevel_handle_v1*,
                           wl_output*) { ++static_cast<Log*>(p)->entered; },
        .output_leave = [](void*, zwlr_foreign_toplevel_handle_v1*, wl_output*) {},
        .state =
            [](void* p, zwlr_foreign_toplevel_handle_v1*, wl_array* a) {
                auto* s = static_cast<uint32_t*>(a->data);
                static_cast<Log*>(p)->states.assign(s, s + a->size / 4);
            },
        .done = [](void* p, zwlr_foreign_toplevel_handle_v1*) { ++static_cast<Log*>(p)->wlr_done; },
        .closed = [](void* p, zwlr_foreign_toplevel_handle_v1*) { ++static_cast<Log*>(p)->closed; },
        .parent = [](void*, zwlr_foreign_toplevel_handle_v1*, zwlr_foreign_toplevel_handle_v1*) {},
    };
    static const zwlr_foreign_toplevel_manager_v1_listener wl = {
        .toplevel =
            [](void* p, zwlr_foreign_toplevel_manager_v1*, zwlr_foreign_toplevel_handle_v1* h) {
                static_cast<Log*>(p)->wlr = h;
                zwlr_foreign_toplevel_handle_v1_add_listener(h, &wh, p);
            },
        .finished = [](void*, zwlr_foreign_toplevel_manager_v1*) {},
    };
    auto* list = d.bind<ext_foreign_toplevel_list_v1>(&ext_foreign_toplevel_list_v1_interface, 1);
    ext_foreign_toplevel_list_v1_add_listener(list, &el, &log);
    auto* mgr = d.bind<zwlr_foreign_toplevel_manager_v1>(&zwlr_foreign_toplevel_manager_v1_interface, 3);
    zwlr_foreign_toplevel_manager_v1_add_listener(mgr, &wl, &log);
    d.pump();
    EXPECT_EQ(log.ext_title, "Mail");
    EXPECT_EQ(log.wlr_title, "Mail");
    EXPECT_EQ(log.entered, 1);

    h->update({.title = "Inbox (3)", .app_id = "org.mail", .activated = true, .outputs = {&d.output}});
    d.pump();
    EXPECT_EQ(log.ext_title, "Inbox (3)");
    EXPECT_EQ(log.wlr_title, "Inbox (3)");
    EXPECT_EQ(log.states, (std::vector<uint32_t>{ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_ACTIVATED}));

    int activates = 0, closes = 0;
    auto a = h->events.request_activate.connect([&](wl::Seat* s) {
        EXPECT_EQ(s, &d.seat);
        ++activates;
    });
    auto c = h->events.request_close.connect([&] { ++closes; });
    zwlr_foreign_toplevel_handle_v1_activate(log.wlr, d.wseat);
    zwlr_foreign_toplevel_handle_v1_close(log.wlr);
    d.pump();
    EXPECT_EQ(activates, 1);
    EXPECT_EQ(closes, 1);

    ft.destroy(h);
    d.pump();
    EXPECT_EQ(log.closed, 2);  // both protocols heard it
    ext_foreign_toplevel_handle_v1_destroy(log.ext);
    zwlr_foreign_toplevel_handle_v1_destroy(log.wlr);
    ext_foreign_toplevel_list_v1_destroy(list);
    zwlr_foreign_toplevel_manager_v1_destroy(mgr);
    d.pump();
    EXPECT_EQ(d.error(), 0);
}

TEST(WlDesktop, WorkspacesAndTheirRequests) {
    Desk d;
    wl::Workspaces ws(d.server);
    wl::Workspaces::Group* g = ws.add_group(1);
    ws.set_group_outputs(g, {&d.output});
    wl::Workspaces::Workspace* one = ws.add_workspace(g, "1", "Web");
    ws.update(one, "Web", 1, {0}, 1);
    ws.add_workspace(g, "2", "Code");

    struct Log {
        std::vector<std::string> names;
        std::vector<ext_workspace_handle_v1*> handles;
        ext_workspace_group_handle_v1* group = nullptr;
        int done = 0;
    } log;
    static const ext_workspace_handle_v1_listener hl = {
        .id = [](void*, ext_workspace_handle_v1*, const char*) {},
        .name = [](void* p, ext_workspace_handle_v1*,
                   const char* n) { static_cast<Log*>(p)->names.emplace_back(n); },
        .coordinates = [](void*, ext_workspace_handle_v1*, wl_array*) {},
        .state = [](void*, ext_workspace_handle_v1*, uint32_t) {},
        .capabilities = [](void*, ext_workspace_handle_v1*, uint32_t) {},
        .removed = [](void*, ext_workspace_handle_v1*) {},
    };
    static const ext_workspace_group_handle_v1_listener gl = {
        .capabilities = [](void*, ext_workspace_group_handle_v1*, uint32_t) {},
        .output_enter = [](void*, ext_workspace_group_handle_v1*, wl_output*) {},
        .output_leave = [](void*, ext_workspace_group_handle_v1*, wl_output*) {},
        .workspace_enter = [](void*, ext_workspace_group_handle_v1*, ext_workspace_handle_v1*) {},
        .workspace_leave = [](void*, ext_workspace_group_handle_v1*, ext_workspace_handle_v1*) {},
        .removed = [](void*, ext_workspace_group_handle_v1*) {},
    };
    static const ext_workspace_manager_v1_listener ml = {
        .workspace_group =
            [](void* p, ext_workspace_manager_v1*, ext_workspace_group_handle_v1* g) {
                static_cast<Log*>(p)->group = g;
                ext_workspace_group_handle_v1_add_listener(g, &gl, p);
            },
        .workspace =
            [](void* p, ext_workspace_manager_v1*, ext_workspace_handle_v1* w) {
                static_cast<Log*>(p)->handles.push_back(w);
                ext_workspace_handle_v1_add_listener(w, &hl, p);
            },
        .done = [](void* p, ext_workspace_manager_v1*) { ++static_cast<Log*>(p)->done; },
        .finished = [](void*, ext_workspace_manager_v1*) {},
    };
    auto* m = d.bind<ext_workspace_manager_v1>(&ext_workspace_manager_v1_interface, 1);
    ext_workspace_manager_v1_add_listener(m, &ml, &log);
    d.pump();
    EXPECT_EQ(log.names, (std::vector<std::string>{"Web", "Code"}));
    EXPECT_GE(log.done, 1);

    std::vector<wl::Workspaces::Request> got;
    auto c = ws.requests.connect([&](const auto& reqs) { got = reqs; });
    ext_workspace_handle_v1_activate(log.handles[1]);
    ext_workspace_group_handle_v1_create_workspace(log.group, "New");
    d.pump();
    EXPECT_TRUE(got.empty());  // not until commit
    ext_workspace_manager_v1_commit(m);
    d.pump();
    ASSERT_EQ(got.size(), 2u);
    EXPECT_EQ(got[0].kind, wl::Workspaces::Request::Kind::Activate);
    EXPECT_EQ(got[1].kind, wl::Workspaces::Request::Kind::Create);
    EXPECT_EQ(got[1].name, "New");
    for (auto* h : log.handles)
        ext_workspace_handle_v1_destroy(h);
    ext_workspace_group_handle_v1_destroy(log.group);
    ext_workspace_manager_v1_stop(m);
    d.pump();
    ext_workspace_manager_v1_destroy(m);
}

TEST(WlDesktop, OutputConfigurationsAnswerOnce) {
    Desk d;
    wl::OutputManagement om(d.server);
    om.set_heads({{.name = "DP-1", .modes = {{1920, 1080, 180000, true}, {1280, 720, 60000, false}},
                   .enabled = true, .current_mode = 0}});
    struct Log {
        std::vector<zwlr_output_head_v1*> heads;
        std::vector<zwlr_output_mode_v1*> modes;
        uint32_t serial = 0;
        int ok = 0, failed = 0, cancelled = 0;
    } log;
    static const zwlr_output_mode_v1_listener mdl = {
        .size = [](void*, zwlr_output_mode_v1*, int32_t, int32_t) {},
        .refresh = [](void*, zwlr_output_mode_v1*, int32_t) {},
        .preferred = [](void*, zwlr_output_mode_v1*) {},
        .finished = [](void*, zwlr_output_mode_v1*) {},
    };
    static const zwlr_output_head_v1_listener hl = {
        .name = [](void*, zwlr_output_head_v1*, const char*) {},
        .description = [](void*, zwlr_output_head_v1*, const char*) {},
        .physical_size = [](void*, zwlr_output_head_v1*, int32_t, int32_t) {},
        .mode =
            [](void* p, zwlr_output_head_v1*, zwlr_output_mode_v1* m) {
                static_cast<Log*>(p)->modes.push_back(m);
                zwlr_output_mode_v1_add_listener(m, &mdl, p);
            },
        .enabled = [](void*, zwlr_output_head_v1*, int32_t) {},
        .current_mode = [](void*, zwlr_output_head_v1*, zwlr_output_mode_v1*) {},
        .position = [](void*, zwlr_output_head_v1*, int32_t, int32_t) {},
        .transform = [](void*, zwlr_output_head_v1*, int32_t) {},
        .scale = [](void*, zwlr_output_head_v1*, wl_fixed_t) {},
        .finished = [](void*, zwlr_output_head_v1*) {},
        .make = [](void*, zwlr_output_head_v1*, const char*) {},
        .model = [](void*, zwlr_output_head_v1*, const char*) {},
        .serial_number = [](void*, zwlr_output_head_v1*, const char*) {},
        .adaptive_sync = [](void*, zwlr_output_head_v1*, uint32_t) {},
    };
    static const zwlr_output_manager_v1_listener ml = {
        .head =
            [](void* p, zwlr_output_manager_v1*, zwlr_output_head_v1* h) {
                static_cast<Log*>(p)->heads.push_back(h);
                zwlr_output_head_v1_add_listener(h, &hl, p);
            },
        .done = [](void* p, zwlr_output_manager_v1*, uint32_t s) { static_cast<Log*>(p)->serial = s; },
        .finished = [](void*, zwlr_output_manager_v1*) {},
    };
    static const zwlr_output_configuration_v1_listener cl = {
        .succeeded = [](void* p, zwlr_output_configuration_v1*) { ++static_cast<Log*>(p)->ok; },
        .failed = [](void* p, zwlr_output_configuration_v1*) { ++static_cast<Log*>(p)->failed; },
        .cancelled = [](void* p, zwlr_output_configuration_v1*) { ++static_cast<Log*>(p)->cancelled; },
    };
    auto* m = d.bind<zwlr_output_manager_v1>(&zwlr_output_manager_v1_interface, 4);
    zwlr_output_manager_v1_add_listener(m, &ml, &log);
    d.pump();
    ASSERT_EQ(log.heads.size(), 1u);
    ASSERT_EQ(log.modes.size(), 2u);

    std::vector<wl::OutputManagement::HeadConfig> applied;
    auto c = om.apply.connect([&](wl::OutputManagement::Configuration& conf) {
        applied = conf.heads;
        conf.done(!conf.test_only);
    });
    auto* conf = zwlr_output_manager_v1_create_configuration(m, log.serial);
    zwlr_output_configuration_v1_add_listener(conf, &cl, &log);
    auto* ch = zwlr_output_configuration_v1_enable_head(conf, log.heads[0]);
    zwlr_output_configuration_head_v1_set_mode(ch, log.modes[1]);
    zwlr_output_configuration_head_v1_set_scale(ch, wl_fixed_from_double(1.25));
    zwlr_output_configuration_v1_apply(conf);
    d.pump();
    EXPECT_EQ(log.ok, 1);
    ASSERT_EQ(applied.size(), 1u);
    EXPECT_EQ(applied[0].mode->width, 1280);
    EXPECT_DOUBLE_EQ(*applied[0].scale, 1.25);
    zwlr_output_configuration_head_v1_destroy(ch);
    zwlr_output_configuration_v1_destroy(conf);

    // Made for an older state of the screens: cancelled.
    const uint32_t stale = log.serial;
    om.set_heads({{.name = "DP-1", .enabled = false}});
    d.pump();
    auto* conf2 = zwlr_output_manager_v1_create_configuration(m, stale);
    zwlr_output_configuration_v1_add_listener(conf2, &cl, &log);
    zwlr_output_configuration_v1_apply(conf2);
    d.pump();
    EXPECT_EQ(log.cancelled, 1);
    zwlr_output_configuration_v1_destroy(conf2);
    for (auto* h : log.heads)
        zwlr_output_head_v1_release(h);
    for (auto* md : log.modes)
        zwlr_output_mode_v1_release(md);
    zwlr_output_manager_v1_stop(m);
    d.pump();
    zwlr_output_manager_v1_destroy(m);
    d.pump();
    EXPECT_EQ(d.error(), 0);
}

TEST(WlDesktop, GammaIsExclusiveAndResetsWhenGone) {
    Desk d;
    wl::GammaControls gc(d.server);
    gc.set_size(&d.output, 4);
    std::vector<std::vector<uint16_t>> tables;
    auto c = gc.set_gamma.connect([&](wl::Output*, const std::vector<uint16_t>& t) { tables.push_back(t); });
    auto* m = d.bind<zwlr_gamma_control_manager_v1>(&zwlr_gamma_control_manager_v1_interface, 1);
    zwlr_gamma_control_v1* g = zwlr_gamma_control_manager_v1_get_gamma_control(m, d.wout);
    zwlr_gamma_control_v1* g2 = zwlr_gamma_control_manager_v1_get_gamma_control(m, d.wout);
    struct Log {
        uint32_t size = 0;
        int failed = 0;
    } log1, log2;
    static const zwlr_gamma_control_v1_listener gl = {
        .gamma_size = [](void* p, zwlr_gamma_control_v1*, uint32_t s) { static_cast<Log*>(p)->size = s; },
        .failed = [](void* p, zwlr_gamma_control_v1*) { ++static_cast<Log*>(p)->failed; },
    };
    zwlr_gamma_control_v1_add_listener(g, &gl, &log1);
    zwlr_gamma_control_v1_add_listener(g2, &gl, &log2);
    d.pump();
    EXPECT_EQ(log1.size, 4u);
    EXPECT_EQ(log2.failed, 1);

    int fd = memfd_create("gamma", MFD_CLOEXEC);
    std::vector<uint16_t> table(12, 0x8000);
    ASSERT_EQ(write(fd, table.data(), table.size() * 2), ssize_t(table.size() * 2));
    zwlr_gamma_control_v1_set_gamma(g, fd);
    close(fd);
    d.pump();
    ASSERT_EQ(tables.size(), 1u);
    EXPECT_EQ(tables[0], table);
    zwlr_gamma_control_v1_destroy(g);
    d.pump();
    ASSERT_EQ(tables.size(), 2u);
    EXPECT_TRUE(tables[1].empty());
    zwlr_gamma_control_v1_destroy(g2);
    zwlr_gamma_control_manager_v1_destroy(m);
}

TEST(WlDesktop, ExportedWindowsParentImportedDialogs) {
    Desk d;
    wl::Shell shell(d.server);
    wl::XdgForeign foreign(d.server);
    std::vector<wl::Toplevel*> made;
    auto c = shell.events.new_toplevel.connect([&](wl::Toplevel* t) { made.push_back(t); });
    auto* comp = d.bind<wl_compositor>(&wl_compositor_interface);
    auto* base = d.bind<xdg_wm_base>(&xdg_wm_base_interface);
    auto window = [&] {
        wl_surface* s = wl_compositor_create_surface(comp);
        xdg_surface* x = xdg_wm_base_get_xdg_surface(base, s);
        xdg_toplevel* t = xdg_surface_get_toplevel(x);
        return std::make_tuple(s, x, t);
    };
    auto [s1, x1, t1] = window();
    auto [s2, x2, t2] = window();
    auto* exporter = d.bind<zxdg_exporter_v2>(&zxdg_exporter_v2_interface, 1);
    auto* importer = d.bind<zxdg_importer_v2>(&zxdg_importer_v2_interface, 1);
    zxdg_exported_v2* e = zxdg_exporter_v2_export_toplevel(exporter, s1);
    std::string handle;
    static const zxdg_exported_v2_listener exl = {
        .handle = [](void* p, zxdg_exported_v2*, const char* h) { *static_cast<std::string*>(p) = h; },
    };
    zxdg_exported_v2_add_listener(e, &exl, &handle);
    d.pump();
    ASSERT_FALSE(handle.empty());
    std::pair<wl::Toplevel*, wl::Toplevel*> parented{};
    auto c2 = foreign.set_parent.connect([&](wl::Toplevel* child, wl::Toplevel* parent) {
        parented = {child, parent};
    });
    zxdg_imported_v2* im = zxdg_importer_v2_import_toplevel(importer, handle.c_str());
    zxdg_imported_v2_set_parent_of(im, s2);
    d.pump();
    EXPECT_EQ(parented.first, made[1]);
    EXPECT_EQ(parented.second, made[0]);
    zxdg_imported_v2_destroy(im);
    zxdg_exported_v2_destroy(e);
    zxdg_importer_v2_destroy(importer);
    zxdg_exporter_v2_destroy(exporter);
    for (auto [s, x, t] : {std::make_tuple(s1, x1, t1), std::make_tuple(s2, x2, t2)}) {
        xdg_toplevel_destroy(t);
        xdg_surface_destroy(x);
        wl_surface_destroy(s);
    }
    xdg_wm_base_destroy(base);
    wl_compositor_destroy(comp);
    d.pump();
    EXPECT_EQ(d.error(), 0);
}

TEST(WlDesktop, GlobalShortcutsRegisterAndFire) {
    Desk d;
    wl::GlobalShortcuts gs(d.server);
    auto* m = d.bind<hyprland_global_shortcuts_manager_v1>(&hyprland_global_shortcuts_manager_v1_interface, 1);
    auto* s = hyprland_global_shortcuts_manager_v1_register_shortcut(m, "mute", "org.voice", "Mute", "Ctrl+M");
    int pressed = 0;
    static const hyprland_global_shortcut_v1_listener sl = {
        .pressed = [](void* p, hyprland_global_shortcut_v1*, uint32_t, uint32_t,
                      uint32_t) { ++*static_cast<int*>(p); },
        .released = [](void*, hyprland_global_shortcut_v1*, uint32_t, uint32_t, uint32_t) {},
    };
    hyprland_global_shortcut_v1_add_listener(s, &sl, &pressed);
    d.pump();
    ASSERT_EQ(gs.shortcuts().size(), 1u);
    EXPECT_EQ(gs.shortcuts()[0]->description, "Mute");
    gs.press(gs.shortcuts()[0].get(), 5'000'000'000ull);
    d.pump();
    EXPECT_EQ(pressed, 1);
    auto* dup = hyprland_global_shortcuts_manager_v1_register_shortcut(m, "mute", "org.voice", "Mute", "");
    d.pump();
    EXPECT_TRUE(d.posted("hyprland_global_shortcuts_manager_v1", HYPRLAND_GLOBAL_SHORTCUTS_MANAGER_V1_ERROR_ALREADY_TAKEN));
    hyprland_global_shortcut_v1_destroy(dup);
    hyprland_global_shortcut_v1_destroy(s);
    hyprland_global_shortcuts_manager_v1_destroy(m);
}

TEST(WlDesktop, SecurityContextTagsItsClients) {
    Desk d;
    wl::SecurityContexts sc(d.server);
    auto* m = d.bind<wp_security_context_manager_v1>(&wp_security_context_manager_v1_interface, 1);
    // The sandbox's socket, and the pipe whose closing ends the context.
    char dir[] = "/tmp/atrium-sc-XXXXXX";
    ASSERT_NE(mkdtemp(dir), nullptr);
    const std::string path = std::string(dir) + "/wayland-sandbox";
    int listen_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::snprintf(addr.sun_path, sizeof addr.sun_path, "%s", path.c_str());
    ASSERT_EQ(bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr), 0);
    ASSERT_EQ(listen(listen_fd, 4), 0);
    int closer[2];
    ASSERT_EQ(pipe2(closer, O_CLOEXEC), 0);
    wp_security_context_v1* ctx = wp_security_context_manager_v1_create_listener(m, listen_fd, closer[0]);
    close(listen_fd);
    close(closer[0]);
    wp_security_context_v1_set_sandbox_engine(ctx, "org.flatpak");
    wp_security_context_v1_set_app_id(ctx, "org.example.App");
    wp_security_context_v1_commit(ctx);
    d.pump();
    ASSERT_EQ(d.error(), 0);

    int sock = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    ASSERT_EQ(connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof addr), 0) << strerror(errno);
    wl_display* sandboxed = wl_display_connect_to_fd(sock);
    ASSERT_NE(sandboxed, nullptr);
    wl_display_flush(sandboxed);
    d.pump_server();
    // The newest client is the sandboxed one.
    wl_client* newest = nullptr;
    wl_client* c = nullptr;
    wl_client_for_each(c, wl_display_get_client_list(d.server)) newest = c;
    ASSERT_NE(newest, d.peer);
    const wl::SecurityContexts::Metadata* meta = sc.lookup(newest);
    ASSERT_NE(meta, nullptr);
    EXPECT_EQ(meta->app_id, "org.example.App");
    EXPECT_EQ(sc.lookup(d.peer), nullptr);

    wl_display_disconnect(sandboxed);
    close(closer[1]);
    d.pump_server();
    wp_security_context_v1_destroy(ctx);
    wp_security_context_manager_v1_destroy(m);
    d.pump();
    unlink(path.c_str());
    rmdir(dir);
    EXPECT_EQ(d.error(), 0);
}
