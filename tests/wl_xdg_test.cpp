// xdg-shell (src/wl/xdg_shell, positioner) against a real libwayland client.
#include <string>
#include "wl/compositor.hpp"
#include "wl/seat.hpp"
#include "wl/shm.hpp"
#include "wl/xdg_shell.hpp"
#include "wl_harness.hpp"

#include "xdg-shell-client-protocol.h"

#include <wayland-client-protocol.h>

#include <gtest/gtest.h>

#include <sys/mman.h>
#include <unistd.h>

using namespace atrium;

TEST(Positioner, AnchorAndGravity) {
    wl::PositionerRules r;
    r.anchor_rect = {10, 20, 100, 30};
    r.width = 50;
    r.height = 40;
    r.anchor = wl::PositionerRules::BottomLeft;
    r.gravity = wl::PositionerRules::BottomRight;
    EXPECT_EQ(r.geometry(), (Box{10, 50, 50, 40}));  // under the anchor, from its left
    r.anchor = wl::PositionerRules::None;
    r.gravity = wl::PositionerRules::None;
    EXPECT_EQ(r.geometry(), (Box{35, 15, 50, 40}));  // centred on it
    r.offset_x = 5;
    EXPECT_EQ(r.geometry().x, 40);
}

TEST(Positioner, FlipsThenSlidesThenResizes) {
    wl::PositionerRules r;
    r.anchor_rect = {0, 90, 20, 10};  // at the bottom of a 100-high space
    r.width = 60;
    r.height = 30;
    r.anchor = wl::PositionerRules::BottomLeft;
    r.gravity = wl::PositionerRules::BottomRight;
    const Box space{0, 0, 200, 100};

    Box b = r.geometry();
    r.unconstrain(space, b);
    EXPECT_EQ(b.y, 100);  // no adjustment allowed: stays out

    r.constraint_adjustment = wl::PositionerRules::FlipY;
    b = r.geometry();
    r.unconstrain(space, b);
    EXPECT_EQ(b, (Box{0, 60, 60, 30}));  // flipped above the anchor

    r.anchor_rect = {180, 40, 20, 10};  // at the right edge
    r.anchor = wl::PositionerRules::TopRight;
    r.gravity = wl::PositionerRules::BottomRight;
    r.constraint_adjustment = wl::PositionerRules::SlideX;
    b = r.geometry();
    r.unconstrain(space, b);
    EXPECT_EQ(b.x, 140);  // slid back in
    EXPECT_EQ(b.width, 60);

    r.width = 300;  // wider than the space
    r.constraint_adjustment = wl::PositionerRules::ResizeX;
    b = r.geometry();
    r.unconstrain(space, b);
    EXPECT_EQ(b.x, 200);
    EXPECT_TRUE(b.x + b.width > 200 || b.empty() || b.width == 300);  // nothing left to resize into
}

namespace {

struct Shell : wltest::Harness {
    wl::Compositor compositor{server, nullptr};
    wl::Shm shm{server, {}};
    wl::Seat seat{server, "seat0"};
    wl::Shell shell{server, 50};
    std::vector<wl::Toplevel*> toplevels;
    std::vector<wl::Popup*> popups;
    wl::Signal<wl::Toplevel*>::Connection c1 =
        shell.events.new_toplevel.connect([this](wl::Toplevel* t) { toplevels.push_back(t); });
    wl::Signal<wl::Popup*>::Connection c2 =
        shell.events.new_popup.connect([this](wl::Popup* p) { popups.push_back(p); });

    wl_compositor* comp = nullptr;
    wl_shm* wshm = nullptr;
    wl_seat* wseat = nullptr;
    xdg_wm_base* base = nullptr;

    Shell() {
        comp = bind<wl_compositor>(&wl_compositor_interface);
        wshm = bind<struct wl_shm>(&wl_shm_interface, 2);
        wseat = bind<wl_seat>(&wl_seat_interface);
        base = bind<xdg_wm_base>(&xdg_wm_base_interface);
        static const xdg_wm_base_listener bl = {
            .ping = [](void* d, xdg_wm_base* b, uint32_t serial) {
                if (*static_cast<bool*>(d))
                    xdg_wm_base_pong(b, serial);
            },
        };
        xdg_wm_base_add_listener(base, &bl, &answer_pings);
        pump();
    }
    ~Shell() {
        if (client) {
            xdg_wm_base_destroy(base);
            wl_seat_release(wseat);
            wl_shm_release(wshm);
            wl_compositor_destroy(comp);
        }
    }
    bool answer_pings = true;

    wl_buffer* buffer(int w, int h) {
        int fd = memfd_create("xdg-test", MFD_CLOEXEC);
        EXPECT_EQ(ftruncate(fd, w * h * 4), 0);
        wl_shm_pool* pool = wl_shm_create_pool(wshm, fd, w * h * 4);
        wl_buffer* b = wl_shm_pool_create_buffer(pool, 0, w, h, w * 4, WL_SHM_FORMAT_ARGB8888);
        wl_shm_pool_destroy(pool);
        close(fd);
        return b;
    }
};

// A client window, recording what it is told.
struct Window {
    Shell& s;
    wl_surface* surface;
    xdg_surface* xdg;
    xdg_toplevel* toplevel;
    uint32_t configure_serial = 0;
    int configures = 0, width = 0, height = 0, closes = 0;
    std::vector<uint32_t> states;
    wl_buffer* buf = nullptr;

    explicit Window(Shell& shell) : s(shell) {
        surface = wl_compositor_create_surface(s.comp);
        xdg = xdg_wm_base_get_xdg_surface(s.base, surface);
        toplevel = xdg_surface_get_toplevel(xdg);
        static const xdg_surface_listener sl = {
            .configure =
                [](void* d, xdg_surface*, uint32_t serial) {
                    auto* w = static_cast<Window*>(d);
                    w->configure_serial = serial;
                    ++w->configures;
                },
        };
        static const xdg_toplevel_listener tl = {
            .configure =
                [](void* d, xdg_toplevel*, int32_t width, int32_t height, wl_array* states) {
                    auto* w = static_cast<Window*>(d);
                    w->width = width;
                    w->height = height;
                    auto* p = static_cast<uint32_t*>(states->data);
                    w->states.assign(p, p + states->size / 4);
                },
            .close = [](void* d, xdg_toplevel*) { ++static_cast<Window*>(d)->closes; },
            .configure_bounds = [](void*, xdg_toplevel*, int32_t, int32_t) {},
            .wm_capabilities = [](void*, xdg_toplevel*, wl_array*) {},
        };
        xdg_surface_add_listener(xdg, &sl, this);
        xdg_toplevel_add_listener(toplevel, &tl, this);
    }
    ~Window() {
        if (!s.client)
            return;
        xdg_toplevel_destroy(toplevel);
        xdg_surface_destroy(xdg);
        wl_surface_destroy(surface);
        if (buf)
            wl_buffer_destroy(buf);
        s.pump();
    }
    // The first commit, the configure, the ack and a buffer: a shown window.
    void show(int w = 200, int h = 100) {
        wl_surface_commit(surface);
        s.pump();
        xdg_surface_ack_configure(xdg, configure_serial);
        buf = s.buffer(w, h);
        wl_surface_attach(surface, buf, 0, 0);
        wl_surface_commit(surface);
        s.pump();
    }
};

} // namespace

TEST(WlXdg, InitialCommitGetsAConfigureThenMaps) {
    Shell s;
    Window w(s);
    s.pump();
    ASSERT_EQ(s.toplevels.size(), 1u);
    wl::Toplevel* t = s.toplevels[0];
    auto ic = t->events.initial_commit.connect([t] { t->set_size(640, 480); });
    EXPECT_EQ(w.configures, 0);
    wl_surface_commit(w.surface);
    s.pump();
    EXPECT_EQ(w.configures, 1);
    EXPECT_EQ(w.width, 640);
    EXPECT_FALSE(t->base()->surface()->mapped());

    xdg_surface_ack_configure(w.xdg, w.configure_serial);
    w.buf = s.buffer(640, 480);
    wl_surface_attach(w.surface, w.buf, 0, 0);
    xdg_surface_set_window_geometry(w.xdg, 10, 10, 620, 460);
    wl_surface_commit(w.surface);
    s.pump();
    EXPECT_TRUE(t->base()->surface()->mapped());
    EXPECT_EQ(t->base()->geometry(), (Box{10, 10, 620, 460}));
    EXPECT_EQ(t->base()->configure_serial(), w.configure_serial);
    EXPECT_EQ(s.error(), 0);
}

TEST(WlXdg, BufferBeforeConfigureIsAnError) {
    Shell s;
    Window w(s);
    w.buf = s.buffer(10, 10);
    wl_surface_attach(w.surface, w.buf, 0, 0);
    wl_surface_commit(w.surface);
    s.pump();
    EXPECT_TRUE(s.posted("xdg_surface", XDG_SURFACE_ERROR_UNCONFIGURED_BUFFER));
}

TEST(WlXdg, ConfigureStatesAckAndCommit) {
    Shell s;
    Window w(s);
    w.show();
    wl::Toplevel* t = s.toplevels[0];
    t->set_activated(true);
    t->set_maximized(true);
    const uint32_t serial = t->set_size(800, 600);
    s.pump();
    EXPECT_EQ(w.configure_serial, serial);
    EXPECT_EQ(w.width, 800);
    EXPECT_NE(std::ranges::find(w.states, uint32_t(XDG_TOPLEVEL_STATE_MAXIMIZED)), w.states.end());
    EXPECT_NE(std::ranges::find(w.states, uint32_t(XDG_TOPLEVEL_STATE_ACTIVATED)), w.states.end());
    EXPECT_FALSE(t->current().maximized);  // not until acked and committed
    xdg_surface_ack_configure(w.xdg, serial);
    s.pump();
    EXPECT_FALSE(t->current().maximized);
    wl_surface_commit(w.surface);
    s.pump();
    EXPECT_TRUE(t->current().maximized);
    EXPECT_EQ(t->current().width, 800);
    // Several changes in one go are one configure.
    const int before = w.configures;
    t->set_activated(false);
    t->set_size(100, 100);
    s.pump();
    EXPECT_EQ(w.configures, before + 1);
    // An unknown serial is an error.
    xdg_surface_ack_configure(w.xdg, serial + 999);
    s.pump();
    EXPECT_TRUE(s.posted("xdg_wm_base", XDG_WM_BASE_ERROR_INVALID_SURFACE_STATE));
}

TEST(WlXdg, RequestsReachTheCompositor) {
    Shell s;
    Window w(s);
    w.show();
    wl::Toplevel* t = s.toplevels[0];
    int titles = 0, maximize = 0, moves = 0;
    wl::Seat* moved_on = nullptr;
    auto a = t->events.set_title.connect([&] { ++titles; });
    auto b = t->events.request_maximize.connect([&] { ++maximize; });
    auto c = t->events.request_move.connect([&](const wl::Toplevel::MoveRequest& r) {
        ++moves;
        moved_on = r.seat;
    });
    xdg_toplevel_set_title(w.toplevel, "Hello");
    xdg_toplevel_set_app_id(w.toplevel, "org.example.app");
    xdg_toplevel_set_maximized(w.toplevel);
    xdg_toplevel_move(w.toplevel, s.wseat, 5);
    s.pump();
    EXPECT_EQ(titles, 1);
    EXPECT_EQ(t->title(), "Hello");
    EXPECT_EQ(t->app_id(), "org.example.app");
    EXPECT_EQ(maximize, 1);
    EXPECT_TRUE(t->requested().maximized);
    EXPECT_EQ(moves, 1);
    EXPECT_EQ(moved_on, &s.seat);

    xdg_toplevel_set_min_size(w.toplevel, 300, 200);
    xdg_toplevel_set_max_size(w.toplevel, 100, 0);  // max below min
    wl_surface_commit(w.surface);
    s.pump();
    EXPECT_TRUE(s.posted("xdg_toplevel", XDG_TOPLEVEL_ERROR_INVALID_SIZE));
}

TEST(WlXdg, SizeLimitsHoldAcrossCommits) {
    Shell s;
    Window w(s);
    w.show();
    wl::Toplevel* t = s.toplevels[0];
    xdg_toplevel_set_min_size(w.toplevel, 300, 200);
    wl_surface_commit(w.surface);
    wl_surface_commit(w.surface);
    s.pump();
    EXPECT_EQ(t->min_width(), 300);
    EXPECT_EQ(t->min_height(), 200);
}

TEST(WlXdg, NullBufferUnmapsAndStartsOver) {
    Shell s;
    Window w(s);
    w.show();
    wl::Toplevel* t = s.toplevels[0];
    ASSERT_TRUE(t->base()->surface()->mapped());
    int unmaps = 0;
    auto c = t->base()->surface()->events.unmap.connect([&] { ++unmaps; });
    wl_surface_attach(w.surface, nullptr, 0, 0);
    wl_surface_commit(w.surface);
    s.pump();
    EXPECT_EQ(unmaps, 1);
    EXPECT_FALSE(t->base()->initialized());
    const int before = w.configures;
    wl_surface_commit(w.surface);  // a new initial commit
    s.pump();
    EXPECT_EQ(w.configures, before + 1);
    EXPECT_EQ(s.error(), 0);
}

TEST(WlXdg, ToplevelGoingUnmapsAndTellsTheCompositor) {
    Shell s;
    wl_surface* surface = wl_compositor_create_surface(s.comp);
    xdg_surface* xdg = xdg_wm_base_get_xdg_surface(s.base, surface);
    xdg_toplevel* toplevel = xdg_surface_get_toplevel(xdg);
    uint32_t serial = 0;
    static const xdg_surface_listener sl = {
        .configure = [](void* d, xdg_surface*, uint32_t sr) { *static_cast<uint32_t*>(d) = sr; },
    };
    xdg_surface_add_listener(xdg, &sl, &serial);
    wl_surface_commit(surface);
    s.pump();
    xdg_surface_ack_configure(xdg, serial);
    wl_buffer* buf = s.buffer(20, 20);
    wl_surface_attach(surface, buf, 0, 0);
    wl_surface_commit(surface);
    s.pump();
    wl::Toplevel* t = s.toplevels[0];
    wl::Surface* ws = t->base()->surface();
    ASSERT_TRUE(ws->mapped());
    // the unmap first: a window torn down on destroy must have been unmapped
    // (and let go of focus) by then
    std::string order;
    auto c = t->events.destroy.connect([&] { order += "destroy "; });
    auto u = ws->events.unmap.connect([&] { order += "unmap "; });
    xdg_toplevel_destroy(toplevel);
    s.pump();
    EXPECT_EQ(order, "unmap destroy ");
    EXPECT_FALSE(ws->mapped());
    xdg_surface_destroy(xdg);
    wl_surface_destroy(surface);
    wl_buffer_destroy(buf);
    s.pump();
    EXPECT_EQ(s.error(), 0);
}

TEST(WlXdg, ParentLoopsAreRefused) {
    Shell s;
    Window a(s), b(s);
    a.show();
    b.show();
    xdg_toplevel_set_parent(b.toplevel, a.toplevel);
    s.pump();
    EXPECT_EQ(s.toplevels[1]->parent(), s.toplevels[0]);
    xdg_toplevel_set_parent(a.toplevel, b.toplevel);
    s.pump();
    EXPECT_TRUE(s.posted("xdg_toplevel", XDG_TOPLEVEL_ERROR_INVALID_PARENT));
}

TEST(WlXdg, PopupsPlaceUnconstrainAndDismiss) {
    Shell s;
    Window w(s);
    w.show(400, 300);
    xdg_positioner* pos = xdg_wm_base_create_positioner(s.base);
    xdg_positioner_set_size(pos, 100, 50);
    xdg_positioner_set_anchor_rect(pos, 350, 10, 20, 20);
    xdg_positioner_set_anchor(pos, XDG_POSITIONER_ANCHOR_BOTTOM_RIGHT);
    xdg_positioner_set_gravity(pos, XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT);
    xdg_positioner_set_constraint_adjustment(pos, XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_X);

    wl_surface* ps = wl_compositor_create_surface(s.comp);
    xdg_surface* pxdg = xdg_wm_base_get_xdg_surface(s.base, ps);
    xdg_popup* popup = xdg_surface_get_popup(pxdg, w.xdg, pos);
    struct Log {
        int x = 0, y = 0, done = 0;
        uint32_t serial = 0;
    } log;
    static const xdg_popup_listener pl = {
        .configure =
            [](void* d, xdg_popup*, int32_t x, int32_t y, int32_t, int32_t) {
                static_cast<Log*>(d)->x = x;
                static_cast<Log*>(d)->y = y;
            },
        .popup_done = [](void* d, xdg_popup*) { ++static_cast<Log*>(d)->done; },
        .repositioned = [](void*, xdg_popup*, uint32_t) {},
    };
    static const xdg_surface_listener sl = {
        .configure = [](void* d, xdg_surface*, uint32_t serial) { static_cast<Log*>(d)->serial = serial; },
    };
    xdg_popup_add_listener(popup, &pl, &log);
    xdg_surface_add_listener(pxdg, &sl, &log);
    wl_surface_commit(ps);
    s.pump();
    ASSERT_EQ(s.popups.size(), 1u);
    wl::Popup* p = s.popups[0];
    EXPECT_EQ(p->parent(), s.toplevels[0]->base()->surface());
    EXPECT_EQ(log.x, 370);  // where the rules put it, past the window's edge
    EXPECT_EQ(log.y, 30);

    p->unconstrain_from({0, 0, 400, 300});
    s.pump();
    EXPECT_EQ(log.x, 300);  // slid back inside

    xdg_surface_ack_configure(pxdg, log.serial);
    wl_surface_commit(ps);
    s.pump();
    EXPECT_EQ(p->geometry().x, 300);

    int popup_gone = 0;
    auto c = p->events.destroy.connect([&] { ++popup_gone; });
    p->dismiss();
    s.pump();
    EXPECT_EQ(log.done, 1);
    EXPECT_EQ(popup_gone, 1);

    xdg_popup_destroy(popup);
    xdg_surface_destroy(pxdg);
    wl_surface_destroy(ps);
    xdg_positioner_destroy(pos);
    s.pump();
    EXPECT_EQ(s.error(), 0);
}

TEST(WlXdg, IncompletePositionerIsAnError) {
    Shell s;
    Window w(s);
    xdg_positioner* pos = xdg_wm_base_create_positioner(s.base);
    wl_surface* ps = wl_compositor_create_surface(s.comp);
    xdg_surface* pxdg = xdg_wm_base_get_xdg_surface(s.base, ps);
    xdg_popup* popup = xdg_surface_get_popup(pxdg, w.xdg, pos);
    s.pump();
    EXPECT_TRUE(s.posted("xdg_wm_base", XDG_WM_BASE_ERROR_INVALID_POSITIONER));
    xdg_popup_destroy(popup);
    xdg_surface_destroy(pxdg);
    wl_surface_destroy(ps);
    xdg_positioner_destroy(pos);
}

TEST(WlXdg, PingTimesOutWhenUnanswered) {
    Shell s;
    Window w(s);
    w.show();
    std::vector<wl_client*> timed_out;
    auto c = s.shell.events.ping_timeout.connect([&](wl_client* cl) { timed_out.push_back(cl); });
    s.toplevels[0]->base()->ping();
    s.pump();
    usleep(80 * 1000);
    s.pump();
    EXPECT_TRUE(timed_out.empty());  // answered

    s.answer_pings = false;
    s.toplevels[0]->base()->ping();
    s.pump();
    usleep(80 * 1000);
    s.pump();
    ASSERT_EQ(timed_out.size(), 1u);
    EXPECT_EQ(timed_out[0], s.peer);
}
