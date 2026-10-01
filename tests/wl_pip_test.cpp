// xx_pip_v1 (src/wl/xdg_shell's Pip) against a real libwayland client.
#include "wl/compositor.hpp"
#include "wl/seat.hpp"
#include "wl/shm.hpp"
#include "wl/xdg_shell.hpp"
#include "wl_harness.hpp"

#include "xdg-shell-client-protocol.h"
#include "xx-pip-v1-client-protocol.h"

#include <wayland-client-protocol.h>

#include <gtest/gtest.h>

#include <sys/mman.h>
#include <unistd.h>

using namespace atrium;

namespace {

struct PipHarness : wltest::Harness {
    wl::Compositor compositor{server, nullptr};
    wl::Shm shm{server, {}};
    wl::Seat seat{server, "seat0"};
    wl::Shell shell{server, 1000};
    wl::PipShell pips{server};
    std::vector<wl::Pip*> made;
    wl::Connection c = pips.events.new_pip.connect([this](wl::Pip* p) { made.push_back(p); });

    wl_compositor* comp = nullptr;
    wl_shm* wshm = nullptr;
    xdg_wm_base* base = nullptr;
    xx_pip_shell_v1* pip_shell = nullptr;

    PipHarness() {
        comp = bind<wl_compositor>(&wl_compositor_interface);
        wshm = bind<struct wl_shm>(&wl_shm_interface, 2);
        base = bind<xdg_wm_base>(&xdg_wm_base_interface);
        pip_shell = bind<xx_pip_shell_v1>(&xx_pip_shell_v1_interface, 1);
        pump();
    }
    ~PipHarness() {
        if (!client)
            return;
        xx_pip_shell_v1_destroy(pip_shell);
        xdg_wm_base_destroy(base);
        wl_shm_release(wshm);
        wl_compositor_destroy(comp);
        pump();
    }
    wl_buffer* buffer(int w, int h) {
        int fd = memfd_create("pip-test", MFD_CLOEXEC);
        EXPECT_EQ(ftruncate(fd, w * h * 4), 0);
        wl_shm_pool* pool = wl_shm_create_pool(wshm, fd, w * h * 4);
        wl_buffer* b = wl_shm_pool_create_buffer(pool, 0, w, h, w * 4, WL_SHM_FORMAT_ARGB8888);
        wl_shm_pool_destroy(pool);
        close(fd);
        return b;
    }
};

// A client's picture-in-picture window, recording what it is told.
struct PipWindow {
    PipHarness& h;
    wl_surface* surface;
    xdg_surface* xdg;
    xx_pip_v1* pip = nullptr;
    wl_buffer* buf = nullptr;
    uint32_t serial = 0;
    int configures = 0, closed = 0, width = -1, height = -1, bounds_w = 0, bounds_h = 0;

    explicit PipWindow(PipHarness& harness, bool make_pip = true) : h(harness) {
        surface = wl_compositor_create_surface(h.comp);
        xdg = xdg_wm_base_get_xdg_surface(h.base, surface);
        static const xdg_surface_listener sl = {
            .configure =
                [](void* d, xdg_surface*, uint32_t serial) {
                    static_cast<PipWindow*>(d)->serial = serial;
                    ++static_cast<PipWindow*>(d)->configures;
                },
        };
        xdg_surface_add_listener(xdg, &sl, this);
        if (make_pip)
            get_pip();
    }
    void get_pip() {
        pip = xx_pip_shell_v1_get_pip(h.pip_shell, xdg);
        static const xx_pip_v1_listener pl = {
            .closed = [](void* d, xx_pip_v1*) { ++static_cast<PipWindow*>(d)->closed; },
            .configure_bounds =
                [](void* d, xx_pip_v1*, int32_t w, int32_t hh) {
                    static_cast<PipWindow*>(d)->bounds_w = w;
                    static_cast<PipWindow*>(d)->bounds_h = hh;
                },
            .configure_size =
                [](void* d, xx_pip_v1*, int32_t w, int32_t hh) {
                    static_cast<PipWindow*>(d)->width = w;
                    static_cast<PipWindow*>(d)->height = hh;
                },
        };
        xx_pip_v1_add_listener(pip, &pl, this);
    }
    ~PipWindow() {
        if (!h.client)
            return;
        if (pip)
            xx_pip_v1_destroy(pip);
        xdg_surface_destroy(xdg);
        wl_surface_destroy(surface);
        if (buf)
            wl_buffer_destroy(buf);
        h.pump();
    }
    void show(int w, int hh) {
        wl_surface_commit(surface);
        h.pump();
        xdg_surface_ack_configure(xdg, serial);
        buf = h.buffer(w, hh);
        wl_surface_attach(surface, buf, 0, 0);
        wl_surface_commit(surface);
        h.pump();
    }
};

} // namespace

TEST(WlPip, ConfiguredThenMapped) {
    PipHarness h;
    PipWindow w(h);
    h.pump();
    ASSERT_EQ(h.made.size(), 1u);
    wl::Pip* p = h.made[0];
    wl::Connection ic = p->events.initial_commit.connect([p] {
        p->set_bounds(640, 360);
        p->set_size(320, 180);
    });
    wl_surface_commit(w.surface);
    h.pump();
    EXPECT_EQ(w.configures, 1);
    EXPECT_EQ(w.width, 320);
    EXPECT_EQ(w.bounds_w, 640);
    EXPECT_FALSE(p->base()->surface()->mapped());

    xdg_surface_ack_configure(w.xdg, w.serial);
    w.buf = h.buffer(320, 180);
    wl_surface_attach(w.surface, w.buf, 0, 0);
    wl_surface_commit(w.surface);
    h.pump();
    EXPECT_TRUE(p->base()->surface()->mapped());
    EXPECT_EQ(p->size(), (std::pair{320, 180}));
    EXPECT_EQ(wl::Pip::from(p->base()->surface()), p);
    EXPECT_EQ(h.error(), 0);
}

TEST(WlPip, OriginAppliesWithTheCommit) {
    PipHarness h;
    wl_surface* page = wl_compositor_create_surface(h.comp);
    PipWindow w(h);
    h.pump();
    wl::Pip* p = h.made[0];
    xx_pip_v1_set_origin(w.pip, page);
    xx_pip_v1_set_origin_rect(w.pip, 10, 20, 300, 200);
    h.pump();
    EXPECT_EQ(p->origin(), nullptr);  // not until committed
    w.show(100, 60);
    ASSERT_NE(p->origin(), nullptr);
    ASSERT_TRUE(p->origin_rect());
    EXPECT_EQ(p->origin_rect()->width, 300);

    // The page goes: so does the origin.
    wl_surface_destroy(page);
    h.pump();
    EXPECT_EQ(p->origin(), nullptr);
    EXPECT_EQ(h.error(), 0);
}

TEST(WlPip, ItsOwnOriginIsAnError) {
    PipHarness h;
    PipWindow w(h);
    xx_pip_v1_set_origin(w.pip, w.surface);
    h.pump();
    EXPECT_TRUE(h.posted("xx_pip_v1", XX_PIP_V1_ERROR_INVALID_ORIGIN));
}

TEST(WlPip, OnlyAFreshXdgSurface) {
    {
        // Already a toplevel.
        PipHarness h;
        PipWindow w(h, false);
        xdg_toplevel* t = xdg_surface_get_toplevel(w.xdg);
        w.get_pip();
        h.pump();
        EXPECT_TRUE(h.posted("xx_pip_shell_v1", XX_PIP_SHELL_V1_ERROR_ROLE));
        xdg_toplevel_destroy(t);
    }
    {
        // Already given a buffer.
        PipHarness h;
        PipWindow w(h, false);
        w.buf = h.buffer(10, 10);
        wl_surface_attach(w.surface, w.buf, 0, 0);
        w.get_pip();
        h.pump();
        EXPECT_TRUE(h.posted("xx_pip_shell_v1", XX_PIP_SHELL_V1_ERROR_ALREADY_CONSTRUCTED));
    }
}

TEST(WlPip, BiggerThanItsBoundsIsAnError) {
    PipHarness h;
    PipWindow w(h);
    h.pump();
    wl::Pip* p = h.made[0];
    wl::Connection ic = p->events.initial_commit.connect([p] { p->set_bounds(200, 100); });
    w.show(400, 100);
    EXPECT_TRUE(h.posted("xx_pip_v1", XX_PIP_V1_ERROR_INVALID_SIZE));
}

TEST(WlPip, MoveAndResizeAreAsked) {
    PipHarness h;
    PipWindow w(h);
    h.pump();
    wl::Pip* p = h.made[0];
    w.show(100, 60);
    auto* wseat = h.bind<wl_seat>(&wl_seat_interface, 5);
    int moves = 0;
    uint32_t edges = 0;
    wl::Connection m = p->events.request_move.connect([&](const wl::Pip::MoveRequest&) { ++moves; });
    wl::Connection r = p->events.request_resize.connect([&](const wl::Pip::ResizeRequest& rq) { edges = rq.edges; });
    xx_pip_v1_move(w.pip, wseat, 1);
    xx_pip_v1_resize(w.pip, wseat, 1, XX_PIP_V1_RESIZE_EDGE_BOTTOM_RIGHT);
    h.pump();
    EXPECT_EQ(moves, 1);
    EXPECT_EQ(edges, uint32_t(XX_PIP_V1_RESIZE_EDGE_BOTTOM_RIGHT));
    xx_pip_v1_resize(w.pip, wseat, 1, 3);  // top and bottom at once
    h.pump();
    EXPECT_TRUE(h.posted("xx_pip_v1", XX_PIP_V1_ERROR_INVALID_RESIZE_EDGE));
    wl_seat_release(wseat);
}

TEST(WlPip, DestroyingItUnmaps) {
    PipHarness h;
    PipWindow w(h);
    h.pump();
    wl::Pip* p = h.made[0];
    wl::Surface* s = p->base()->surface();
    w.show(100, 60);
    ASSERT_TRUE(s->mapped());
    bool gone = false;
    wl::Connection d = p->events.destroy.connect([&] { gone = true; });
    xx_pip_v1_destroy(w.pip);
    w.pip = nullptr;
    h.pump();
    EXPECT_TRUE(gone);
    EXPECT_FALSE(s->mapped());
    EXPECT_EQ(h.error(), 0);
}
