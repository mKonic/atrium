// Layer shell, session lock and the xdg extras (src/wl) against a real
// libwayland client.
#include "wl/compositor.hpp"
#include "wl/layer_shell.hpp"
#include "wl/output.hpp"
#include "wl/seat.hpp"
#include "wl/session_lock.hpp"
#include "wl/shm.hpp"
#include "wl/xdg_extras.hpp"
#include "wl/xdg_shell.hpp"
#include "wl_harness.hpp"

#include "ext-session-lock-v1-client-protocol.h"
// The C header names an argument `namespace`.
#define namespace namespace_
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#undef namespace
#include "xdg-activation-v1-client-protocol.h"
#include "xdg-decoration-unstable-v1-client-protocol.h"
#include "xdg-dialog-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"

#include <wayland-client-protocol.h>

#include <gtest/gtest.h>

#include <sys/mman.h>
#include <unistd.h>

using namespace atrium;

namespace {

struct Desk : wltest::Harness {
    wl::Compositor compositor{server, nullptr};
    wl::Shm shm{server, {}};
    wl::Seat seat{server, "seat0"};
    wl::Output output{server, wl::OutputInfo{.name = "DP-1", .mode_width = 800, .mode_height = 600,
                                             .logical_width = 800, .logical_height = 600}};
    wl::Shell shell{server};
    wl::LayerShell layers{server};
    wl::SessionLockManager locks{server};
    wl::Decorations decorations{server};
    wl::Dialogs dialogs{server};
    wl::Activation activation{server, seat};

    wl_compositor* comp = nullptr;
    wl_shm* wshm = nullptr;
    wl_output* wout = nullptr;
    wl_seat* wseat = nullptr;
    std::vector<wl_buffer*> buffers;
    std::vector<wl::Surface*> surfaces;
    wl::Connection made = compositor.new_surface.connect([this](wl::Surface* s) { surfaces.push_back(s); });

    Desk() {
        seat.set_capabilities(wl::Seat::Keyboard);
        comp = bind<wl_compositor>(&wl_compositor_interface);
        wshm = bind<struct wl_shm>(&wl_shm_interface, 2);
        wout = bind<wl_output>(&wl_output_interface, 4);
        wseat = bind<wl_seat>(&wl_seat_interface);
        pump();
    }
    ~Desk() {
        if (!client)
            return;
        for (wl_buffer* b : buffers)
            wl_buffer_destroy(b);
        wl_seat_release(wseat);
        wl_output_release(wout);
        wl_shm_release(wshm);
        wl_compositor_destroy(comp);
        pump();
    }

    wl_buffer* buffer(int w, int h) {
        int fd = memfd_create("shells-test", MFD_CLOEXEC);
        EXPECT_EQ(ftruncate(fd, w * h * 4), 0);
        wl_shm_pool* pool = wl_shm_create_pool(wshm, fd, w * h * 4);
        wl_buffer* b = wl_shm_pool_create_buffer(pool, 0, w, h, w * 4, WL_SHM_FORMAT_ARGB8888);
        wl_shm_pool_destroy(pool);
        close(fd);
        buffers.push_back(b);
        return b;
    }
};

struct LayerLog {
    uint32_t serial = 0, width = 0, height = 0;
    int closed = 0;
};
const zwlr_layer_surface_v1_listener kLayer = {
    .configure =
        [](void* d, zwlr_layer_surface_v1*, uint32_t serial, uint32_t w, uint32_t h) {
            auto* l = static_cast<LayerLog*>(d);
            l->serial = serial;
            l->width = w;
            l->height = h;
        },
    .closed = [](void* d, zwlr_layer_surface_v1*) { ++static_cast<LayerLog*>(d)->closed; },
};

} // namespace

TEST(WlLayer, ConfigureAckMap) {
    Desk d;
    wl::LayerSurface* made = nullptr;
    auto c = d.layers.new_surface.connect([&](wl::LayerSurface* l) {
        made = l;
        EXPECT_EQ(l->output(), &d.output);
    });
    auto* shell = d.bind<zwlr_layer_shell_v1>(&zwlr_layer_shell_v1_interface, 5);
    wl_surface* s = wl_compositor_create_surface(d.comp);
    auto* ls = zwlr_layer_shell_v1_get_layer_surface(shell, s, d.wout, ZWLR_LAYER_SHELL_V1_LAYER_TOP, "bar");
    LayerLog log;
    zwlr_layer_surface_v1_add_listener(ls, &kLayer, &log);
    zwlr_layer_surface_v1_set_size(ls, 0, 30);
    zwlr_layer_surface_v1_set_anchor(ls, ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
                                             ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
    zwlr_layer_surface_v1_set_exclusive_zone(ls, 30);
    d.pump();
    ASSERT_NE(made, nullptr);
    EXPECT_EQ(made->name_space(), "bar");
    auto ic = made->events.initial_commit.connect([&] { made->configure(800, 30); });
    wl_surface_commit(s);
    d.pump();
    EXPECT_EQ(log.width, 800u);
    EXPECT_EQ(made->current().exclusive_zone, 30);
    EXPECT_EQ(made->current().layer, uint32_t(ZWLR_LAYER_SHELL_V1_LAYER_TOP));

    zwlr_layer_surface_v1_ack_configure(ls, log.serial);
    wl_surface_attach(s, d.buffer(800, 30), 0, 0);
    wl_surface_commit(s);
    d.pump();
    EXPECT_TRUE(made->surface()->mapped());
    EXPECT_EQ(made->current().actual_width, 800u);

    // A change that isn't committed doesn't apply; values stay as set.
    zwlr_layer_surface_v1_set_exclusive_zone(ls, 50);
    d.pump();
    EXPECT_EQ(made->current().exclusive_zone, 30);
    wl_surface_commit(s);
    d.pump();
    EXPECT_EQ(made->current().exclusive_zone, 50);
    EXPECT_EQ(made->current().desired_height, 30u);

    made->close();
    d.pump();
    EXPECT_EQ(log.closed, 1);
    zwlr_layer_surface_v1_destroy(ls);
    wl_surface_destroy(s);
    zwlr_layer_shell_v1_destroy(shell);
    d.pump();
    EXPECT_EQ(d.error(), 0);
}

TEST(WlLayer, ZeroWidthNeedsBothSideAnchors) {
    Desk d;
    auto* shell = d.bind<zwlr_layer_shell_v1>(&zwlr_layer_shell_v1_interface, 5);
    wl_surface* s = wl_compositor_create_surface(d.comp);
    auto* ls = zwlr_layer_shell_v1_get_layer_surface(shell, s, nullptr, ZWLR_LAYER_SHELL_V1_LAYER_TOP, "x");
    zwlr_layer_surface_v1_set_size(ls, 0, 30);
    zwlr_layer_surface_v1_set_anchor(ls, ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP);
    wl_surface_commit(s);
    d.pump();
    EXPECT_EQ(d.protocol_error(), uint32_t(ZWLR_LAYER_SURFACE_V1_ERROR_INVALID_SIZE));
    zwlr_layer_surface_v1_destroy(ls);
    wl_surface_destroy(s);
    zwlr_layer_shell_v1_destroy(shell);
}

TEST(WlLock, LockSurfacesMustFitAndUnlockNeedsALock) {
    Desk d;
    wl::Lock* lock = nullptr;
    wl::LockSurface* lsurf = nullptr;
    int unlocked = 0, destroyed = 0;
    std::vector<wl::Connection> conns;
    auto c = d.locks.new_lock.connect([&](wl::Lock* l) {
        lock = l;
        conns.push_back(l->events.new_surface.connect([&](wl::LockSurface* s) {
            lsurf = s;
            s->configure(800, 600);
        }));
        conns.push_back(l->events.unlock.connect([&] { ++unlocked; }));
        conns.push_back(l->events.destroy.connect([&] { ++destroyed; }));
    });
    auto* m = d.bind<ext_session_lock_manager_v1>(&ext_session_lock_manager_v1_interface, 1);
    ext_session_lock_v1* l = ext_session_lock_manager_v1_lock(m);
    int locked = 0;
    static const ext_session_lock_v1_listener ll = {
        .locked = [](void* p, ext_session_lock_v1*) { ++*static_cast<int*>(p); },
        .finished = [](void*, ext_session_lock_v1*) {},
    };
    ext_session_lock_v1_add_listener(l, &ll, &locked);
    wl_surface* s = wl_compositor_create_surface(d.comp);
    ext_session_lock_surface_v1* ls = ext_session_lock_v1_get_lock_surface(l, s, d.wout);
    uint32_t serial = 0;
    static const ext_session_lock_surface_v1_listener sl = {
        .configure = [](void* p, ext_session_lock_surface_v1*, uint32_t sr, uint32_t,
                        uint32_t) { *static_cast<uint32_t*>(p) = sr; },
    };
    ext_session_lock_surface_v1_add_listener(ls, &sl, &serial);
    d.pump();
    ASSERT_NE(lsurf, nullptr);
    ASSERT_NE(serial, 0u);
    ext_session_lock_surface_v1_ack_configure(ls, serial);
    wl_surface_attach(s, d.buffer(800, 600), 0, 0);
    wl_surface_commit(s);
    d.pump();
    EXPECT_TRUE(lsurf->surface()->mapped());
    lock->locked();
    d.pump();
    EXPECT_EQ(locked, 1);

    ext_session_lock_v1_unlock_and_destroy(l);
    d.pump();
    EXPECT_EQ(unlocked, 1);
    EXPECT_EQ(destroyed, 1);
    ext_session_lock_surface_v1_destroy(ls);
    wl_surface_destroy(s);
    ext_session_lock_manager_v1_destroy(m);
    d.pump();
    EXPECT_EQ(d.error(), 0);
}

TEST(WlLock, WrongSizeIsAnError) {
    Desk d;
    auto c = d.locks.new_lock.connect([&](wl::Lock* l) {
        static wl::Connection keep;
        keep = l->events.new_surface.connect([](wl::LockSurface* s) { s->configure(800, 600); });
    });
    auto* m = d.bind<ext_session_lock_manager_v1>(&ext_session_lock_manager_v1_interface, 1);
    ext_session_lock_v1* l = ext_session_lock_manager_v1_lock(m);
    wl_surface* s = wl_compositor_create_surface(d.comp);
    ext_session_lock_surface_v1* ls = ext_session_lock_v1_get_lock_surface(l, s, d.wout);
    uint32_t serial = 0;
    static const ext_session_lock_surface_v1_listener sl = {
        .configure = [](void* p, ext_session_lock_surface_v1*, uint32_t sr, uint32_t,
                        uint32_t) { *static_cast<uint32_t*>(p) = sr; },
    };
    ext_session_lock_surface_v1_add_listener(ls, &sl, &serial);
    d.pump();
    ext_session_lock_surface_v1_ack_configure(ls, serial);
    wl_surface_attach(s, d.buffer(640, 480), 0, 0);
    wl_surface_commit(s);
    d.pump();
    EXPECT_EQ(d.protocol_error(), uint32_t(EXT_SESSION_LOCK_SURFACE_V1_ERROR_DIMENSIONS_MISMATCH));
    ext_session_lock_surface_v1_destroy(ls);
    wl_surface_destroy(s);
    ext_session_lock_v1_destroy(l);
    ext_session_lock_manager_v1_destroy(m);
}

TEST(WlLock, UnlockBeforeLockedIsAnError) {
    Desk d;
    auto* m = d.bind<ext_session_lock_manager_v1>(&ext_session_lock_manager_v1_interface, 1);
    ext_session_lock_v1* l = ext_session_lock_manager_v1_lock(m);
    ext_session_lock_v1_unlock_and_destroy(l);
    d.pump();
    EXPECT_EQ(d.protocol_error(), uint32_t(EXT_SESSION_LOCK_V1_ERROR_INVALID_UNLOCK));
    ext_session_lock_manager_v1_destroy(m);
}

namespace {

// A mapped xdg toplevel.
struct Win {
    Desk& d;
    xdg_wm_base* base;
    wl_surface* surface;
    xdg_surface* xdg;
    xdg_toplevel* toplevel;
    uint32_t serial = 0;
    explicit Win(Desk& desk) : d(desk) {
        base = d.bind<xdg_wm_base>(&xdg_wm_base_interface);
        surface = wl_compositor_create_surface(d.comp);
        xdg = xdg_wm_base_get_xdg_surface(base, surface);
        toplevel = xdg_surface_get_toplevel(xdg);
        static const xdg_surface_listener sl = {
            .configure = [](void* p, xdg_surface*, uint32_t sr) { static_cast<Win*>(p)->serial = sr; },
        };
        xdg_surface_add_listener(xdg, &sl, this);
    }
    ~Win() {
        if (!d.client)
            return;
        xdg_toplevel_destroy(toplevel);
        xdg_surface_destroy(xdg);
        wl_surface_destroy(surface);
        xdg_wm_base_destroy(base);
        d.pump();
    }
};

} // namespace

TEST(WlXdgExtras, DecorationModeGoesWithTheConfigure) {
    Desk d;
    wl::Toplevel* t = nullptr;
    auto c = d.shell.events.new_toplevel.connect([&](wl::Toplevel* x) { t = x; });
    std::vector<wl::Decorations::Xdg*> asked;
    auto c2 = d.decorations.events.request_mode.connect([&](wl::Decorations::Xdg* x) { asked.push_back(x); });
    Win w(d);
    auto* mgr = d.bind<zxdg_decoration_manager_v1>(&zxdg_decoration_manager_v1_interface, 1);
    auto* deco = zxdg_decoration_manager_v1_get_toplevel_decoration(mgr, w.toplevel);
    uint32_t mode = 0;
    static const zxdg_toplevel_decoration_v1_listener dl = {
        .configure = [](void* p, zxdg_toplevel_decoration_v1*, uint32_t m) { *static_cast<uint32_t*>(p) = m; },
    };
    zxdg_toplevel_decoration_v1_add_listener(deco, &dl, &mode);
    zxdg_toplevel_decoration_v1_set_mode(deco, ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE);
    d.pump();
    ASSERT_EQ(asked.size(), 1u);
    EXPECT_EQ(asked[0]->requested, wl::Decorations::ClientSide);
    wl_surface_commit(w.surface);
    d.pump();
    EXPECT_EQ(mode, 0u);  // nothing decided yet
    d.decorations.set_mode(asked[0], wl::Decorations::ServerSide);
    d.pump();
    EXPECT_EQ(mode, uint32_t(ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE));

    // A second decoration object for the same window is an error.
    auto* second = zxdg_decoration_manager_v1_get_toplevel_decoration(mgr, w.toplevel);
    d.pump();
    EXPECT_EQ(d.protocol_error(), uint32_t(ZXDG_TOPLEVEL_DECORATION_V1_ERROR_ALREADY_CONSTRUCTED));
    zxdg_toplevel_decoration_v1_destroy(second);
    zxdg_toplevel_decoration_v1_destroy(deco);
    zxdg_decoration_manager_v1_destroy(mgr);
}

TEST(WlXdgExtras, ModalDialogs) {
    Desk d;
    wl::Toplevel* t = nullptr;
    auto c = d.shell.events.new_toplevel.connect([&](wl::Toplevel* x) { t = x; });
    Win w(d);
    auto* mgr = d.bind<xdg_wm_dialog_v1>(&xdg_wm_dialog_v1_interface, 1);
    xdg_dialog_v1* dialog = xdg_wm_dialog_v1_get_xdg_dialog(mgr, w.toplevel);
    int changes = 0;
    auto c2 = d.dialogs.changed.connect([&](wl::Toplevel*) { ++changes; });
    xdg_dialog_v1_set_modal(dialog);
    d.pump();
    ASSERT_NE(t, nullptr);
    EXPECT_TRUE(d.dialogs.modal(t));
    xdg_dialog_v1_destroy(dialog);
    d.pump();
    EXPECT_FALSE(d.dialogs.modal(t));
    EXPECT_EQ(changes, 2);
    xdg_wm_dialog_v1_destroy(mgr);
}

TEST(WlXdgExtras, ActivationTokensFromRealInputOnly) {
    Desk d;
    wl_surface* s = wl_compositor_create_surface(d.comp);
    d.pump();
    wl::Surface* ss = d.surfaces.back();
    wl_keyboard* kb = wl_seat_get_keyboard(d.wseat);
    uint32_t key_serial = 0;
    static const wl_keyboard_listener kl = {
        .keymap = [](void*, wl_keyboard*, uint32_t, int32_t fd, uint32_t) { close(fd); },
        .enter = [](void*, wl_keyboard*, uint32_t, wl_surface*, wl_array*) {},
        .leave = [](void*, wl_keyboard*, uint32_t, wl_surface*) {},
        .key = [](void* p, wl_keyboard*, uint32_t serial, uint32_t, uint32_t,
                  uint32_t) { *static_cast<uint32_t*>(p) = serial; },
        .modifiers = [](void*, wl_keyboard*, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) {},
        .repeat_info = [](void*, wl_keyboard*, int32_t, int32_t) {},
    };
    wl_keyboard_add_listener(kb, &kl, &key_serial);
    d.pump();
    d.seat.keyboard_enter(ss, {}, {});
    d.seat.keyboard_key(1, 30, true);
    d.pump();
    ASSERT_NE(key_serial, 0u);

    auto* act = d.bind<xdg_activation_v1>(&xdg_activation_v1_interface, 1);
    auto token_for = [&](uint32_t serial) {
        xdg_activation_token_v1* tok = xdg_activation_v1_get_activation_token(act);
        std::string name;
        static const xdg_activation_token_v1_listener tl = {
            .done = [](void* p, xdg_activation_token_v1*, const char* n) { *static_cast<std::string*>(p) = n; },
        };
        xdg_activation_token_v1_add_listener(tok, &tl, &name);
        xdg_activation_token_v1_set_serial(tok, serial, d.wseat);
        xdg_activation_token_v1_commit(tok);
        d.pump();
        xdg_activation_token_v1_destroy(tok);
        return name;
    };
    const std::string real = token_for(key_serial);
    const std::string fake = token_for(key_serial + 50);
    EXPECT_FALSE(real.empty());
    EXPECT_NE(real, fake);

    std::vector<std::pair<wl::Surface*, bool>> asked;  // with a token that may take focus
    auto c = d.activation.request_activate.connect([&](const wl::Activation::Request& r) {
        asked.push_back({r.surface, r.token && r.token->seat});
    });
    xdg_activation_v1_activate(act, real.c_str(), s);
    xdg_activation_v1_activate(act, fake.c_str(), s);
    xdg_activation_v1_activate(act, "nonsense", s);
    d.pump();
    ASSERT_EQ(asked.size(), 3u);
    EXPECT_TRUE(asked[0].second);
    EXPECT_FALSE(asked[1].second);
    EXPECT_FALSE(asked[2].second);
    EXPECT_EQ(asked[0].first, ss);

    xdg_activation_v1_destroy(act);
    wl_keyboard_release(kb);
    wl_surface_destroy(s);
    d.pump();
    EXPECT_EQ(d.error(), 0);
}
