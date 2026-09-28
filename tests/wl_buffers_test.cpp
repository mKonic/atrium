// Buffers, sync and timing (src/wl: dmabuf, surface_ext, timing) against a
// real libwayland client.
#include "wl/compositor.hpp"
#include "wl/dmabuf.hpp"
#include "wl/output.hpp"
#include "wl/shm.hpp"
#include "wl/surface_ext.hpp"
#include "wl/timing.hpp"
#include "wl_harness.hpp"

#include "alpha-modifier-v1-client-protocol.h"
#include "commit-timing-v1-client-protocol.h"
#include "fifo-v1-client-protocol.h"
#include "fractional-scale-v1-client-protocol.h"
#include "linux-dmabuf-v1-client-protocol.h"
#include "linux-drm-syncobj-v1-client-protocol.h"
#include "presentation-time-client-protocol.h"
#include "single-pixel-buffer-v1-client-protocol.h"
#include "viewporter-client-protocol.h"

#include <wayland-client-protocol.h>

#include <gtest/gtest.h>

#include <drm_fourcc.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <xf86drm.h>

#include <ctime>

using namespace atrium;

namespace {

struct Buffers : wltest::Harness {
    wl::Compositor compositor{server, nullptr};
    wl::Shm shm{server, {}};
    wl::Output output{server, wl::OutputInfo{.name = "DP-1"}};
    std::vector<wl::Surface*> surfaces;
    wl::Connection made = compositor.new_surface.connect([this](wl::Surface* s) { surfaces.push_back(s); });
    wl_compositor* comp = nullptr;
    wl_shm* wshm = nullptr;
    std::vector<wl_buffer*> buffers;

    Buffers() {
        comp = bind<wl_compositor>(&wl_compositor_interface);
        wshm = bind<struct wl_shm>(&wl_shm_interface, 2);
        pump();
    }
    ~Buffers() {
        if (!client)
            return;
        for (wl_buffer* b : buffers)
            wl_buffer_destroy(b);
        wl_shm_release(wshm);
        wl_compositor_destroy(comp);
        pump();
    }
    std::pair<wl_surface*, wl::Surface*> surface() {
        wl_surface* s = wl_compositor_create_surface(comp);
        pump();
        return {s, surfaces.back()};
    }
    wl_buffer* buffer(int w, int h) {
        int fd = memfd_create("buffers-test", MFD_CLOEXEC);
        EXPECT_EQ(ftruncate(fd, w * h * 4), 0);
        wl_shm_pool* pool = wl_shm_create_pool(wshm, fd, w * h * 4);
        wl_buffer* b = wl_shm_pool_create_buffer(pool, 0, w, h, w * 4, WL_SHM_FORMAT_ARGB8888);
        wl_shm_pool_destroy(pool);
        close(fd);
        buffers.push_back(b);
        return b;
    }
};

// Something to stand in for a dmabuf plane: the protocol layer only checks
// sizes (the import check is the compositor's).
int fake_plane(size_t size) {
    int fd = memfd_create("fake-dmabuf", MFD_CLOEXEC);
    EXPECT_EQ(ftruncate(fd, off_t(size)), 0);
    return fd;
}

} // namespace

TEST(WlDmabuf, ParamsMakeBuffersAndCheckBounds) {
    Buffers b;
    wl::DmabufFeedback fb;
    fb.main_device = makedev(226, 128);
    fb.tranches.push_back({makedev(226, 128), false, {{DRM_FORMAT_ARGB8888, DRM_FORMAT_MOD_LINEAR}}});
    int checks = 0;
    wl::LinuxDmabuf dmabuf(b.server, fb, [&](const wlr_dmabuf_attributes&) {
        ++checks;
        return true;
    });
    auto* m = b.bind<zwp_linux_dmabuf_v1>(&zwp_linux_dmabuf_v1_interface, 5);

    zwp_linux_buffer_params_v1* p = zwp_linux_dmabuf_v1_create_params(m);
    zwp_linux_buffer_params_v1_add(p, fake_plane(64 * 64 * 4), 0, 0, 64 * 4, 0, 0);
    wl_buffer* buf = zwp_linux_buffer_params_v1_create_immed(p, 64, 64, DRM_FORMAT_ARGB8888, 0);
    zwp_linux_buffer_params_v1_destroy(p);
    auto [s, ss] = b.surface();
    wl_surface_attach(s, buf, 0, 0);
    wl_surface_commit(s);
    b.pump();
    EXPECT_EQ(checks, 1);
    ASSERT_NE(ss->current().buffer.get(), nullptr);
    EXPECT_TRUE(wl::LinuxDmabuf::is_dmabuf(ss->current().buffer.get()));
    EXPECT_EQ(ss->current().width, 64);
    wl_surface_destroy(s);
    wl_buffer_destroy(buf);

    // A stride taller than the plane is out of bounds.
    p = zwp_linux_dmabuf_v1_create_params(m);
    zwp_linux_buffer_params_v1_add(p, fake_plane(64 * 4), 0, 0, 64 * 4, 0, 0);
    wl_buffer* bad = zwp_linux_buffer_params_v1_create_immed(p, 64, 64, DRM_FORMAT_ARGB8888, 0);
    b.pump();
    EXPECT_EQ(b.protocol_error(), uint32_t(ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_OUT_OF_BOUNDS));
    wl_buffer_destroy(bad);
    zwp_linux_buffer_params_v1_destroy(p);
    zwp_linux_dmabuf_v1_destroy(m);
}

TEST(WlDmabuf, RefusedImportFails) {
    Buffers b;
    wl::LinuxDmabuf dmabuf(b.server, {}, [](const wlr_dmabuf_attributes&) { return false; });
    auto* m = b.bind<zwp_linux_dmabuf_v1>(&zwp_linux_dmabuf_v1_interface, 5);
    zwp_linux_buffer_params_v1* p = zwp_linux_dmabuf_v1_create_params(m);
    zwp_linux_buffer_params_v1_add(p, fake_plane(16 * 16 * 4), 0, 0, 16 * 4, 0, 0);
    int failed = 0;
    static const zwp_linux_buffer_params_v1_listener pl = {
        .created = [](void*, zwp_linux_buffer_params_v1*, wl_buffer*) {},
        .failed = [](void* d, zwp_linux_buffer_params_v1*) { ++*static_cast<int*>(d); },
    };
    zwp_linux_buffer_params_v1_add_listener(p, &pl, &failed);
    zwp_linux_buffer_params_v1_create(p, 16, 16, DRM_FORMAT_ARGB8888, 0);
    b.pump();
    EXPECT_EQ(failed, 1);
    EXPECT_EQ(b.error(), 0);
    zwp_linux_buffer_params_v1_destroy(p);
    zwp_linux_dmabuf_v1_destroy(m);
}

TEST(WlDmabuf, FeedbackListsTheTable) {
    Buffers b;
    wl::DmabufFeedback fb;
    fb.main_device = makedev(226, 128);
    fb.tranches.push_back({makedev(226, 0), true, {{DRM_FORMAT_XRGB8888, 0x0100000000000001ull}}});
    fb.tranches.push_back({makedev(226, 128), false,
                           {{DRM_FORMAT_ARGB8888, DRM_FORMAT_MOD_LINEAR}, {DRM_FORMAT_XRGB8888, 0x0100000000000001ull}}});
    wl::LinuxDmabuf dmabuf(b.server, fb, {});
    auto* m = b.bind<zwp_linux_dmabuf_v1>(&zwp_linux_dmabuf_v1_interface, 5);
    struct Log {
        dev_t main = 0;
        std::vector<std::pair<uint32_t, uint64_t>> table;
        std::vector<std::vector<uint16_t>> tranches;
        std::vector<uint32_t> flags;
        int done = 0;
    } log;
    static const zwp_linux_dmabuf_feedback_v1_listener fl = {
        .done = [](void* d, zwp_linux_dmabuf_feedback_v1*) { ++static_cast<Log*>(d)->done; },
        .format_table =
            [](void* d, zwp_linux_dmabuf_feedback_v1*, int32_t fd, uint32_t size) {
                struct E {
                    uint32_t f, pad;
                    uint64_t m;
                };
                void* p = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
                auto* e = static_cast<E*>(p);
                for (size_t i = 0; i < size / sizeof(E); ++i)
                    static_cast<Log*>(d)->table.push_back({e[i].f, e[i].m});
                munmap(p, size);
                close(fd);
            },
        .main_device = [](void* d, zwp_linux_dmabuf_feedback_v1*,
                          wl_array* a) { std::memcpy(&static_cast<Log*>(d)->main, a->data, sizeof(dev_t)); },
        .tranche_done = [](void*, zwp_linux_dmabuf_feedback_v1*) {},
        .tranche_target_device = [](void*, zwp_linux_dmabuf_feedback_v1*, wl_array*) {},
        .tranche_formats =
            [](void* d, zwp_linux_dmabuf_feedback_v1*, wl_array* a) {
                auto* p = static_cast<uint16_t*>(a->data);
                static_cast<Log*>(d)->tranches.emplace_back(p, p + a->size / 2);
            },
        .tranche_flags = [](void* d, zwp_linux_dmabuf_feedback_v1*,
                            uint32_t f) { static_cast<Log*>(d)->flags.push_back(f); },
    };
    zwp_linux_dmabuf_feedback_v1* f = zwp_linux_dmabuf_v1_get_default_feedback(m);
    zwp_linux_dmabuf_feedback_v1_add_listener(f, &fl, &log);
    b.pump();
    EXPECT_EQ(log.done, 1);
    EXPECT_EQ(log.main, makedev(226, 128));
    ASSERT_EQ(log.table.size(), 2u);  // the shared entry listed once
    ASSERT_EQ(log.tranches.size(), 2u);
    EXPECT_EQ(log.tranches[1].size(), 2u);
    EXPECT_EQ(log.table[log.tranches[0][0]].first, uint32_t(DRM_FORMAT_XRGB8888));
    EXPECT_EQ(log.flags, (std::vector<uint32_t>{ZWP_LINUX_DMABUF_FEEDBACK_V1_TRANCHE_FLAGS_SCANOUT, 0}));
    zwp_linux_dmabuf_feedback_v1_destroy(f);
    zwp_linux_dmabuf_v1_destroy(m);
}

TEST(WlSurfaceExt, ViewportCropsAndScales) {
    Buffers b;
    wl::Viewporter vp(b.server);
    auto* m = b.bind<wp_viewporter>(&wp_viewporter_interface, 1);
    auto [s, ss] = b.surface();
    wp_viewport* v = wp_viewporter_get_viewport(m, s);
    wp_viewport_set_destination(v, 300, 200);
    wl_surface_attach(s, b.buffer(100, 100), 0, 0);
    wl_surface_commit(s);
    b.pump();
    EXPECT_EQ(ss->current().width, 300);
    EXPECT_EQ(ss->current().height, 200);

    wp_viewport_set_source(v, wl_fixed_from_double(10), wl_fixed_from_double(10), wl_fixed_from_double(50),
                           wl_fixed_from_double(50));
    wl_surface_commit(s);
    b.pump();
    EXPECT_DOUBLE_EQ(ss->source_box().width, 50);

    // Destroyed, the viewport goes with the next commit.
    wp_viewport_destroy(v);
    wl_surface_commit(s);
    b.pump();
    EXPECT_EQ(ss->current().width, 100);

    wp_viewport* v2 = wp_viewporter_get_viewport(m, s);
    wp_viewport_set_source(v2, 0, 0, wl_fixed_from_double(500), wl_fixed_from_double(50));
    wp_viewport_set_destination(v2, 10, 10);
    wl_surface_commit(s);
    b.pump();
    EXPECT_EQ(b.protocol_error(), uint32_t(WP_VIEWPORT_ERROR_OUT_OF_BUFFER));
    wp_viewport_destroy(v2);
    wp_viewporter_destroy(m);
    wl_surface_destroy(s);
}

TEST(WlSurfaceExt, HintsApplyWithTheCommitAndResetWhenGone) {
    Buffers b;
    wl::SurfaceHints hints(b.server);
    wl::FractionalScales scales(b.server);
    auto* am = b.bind<wp_alpha_modifier_v1>(&wp_alpha_modifier_v1_interface, 1);
    auto* fm = b.bind<wp_fractional_scale_manager_v1>(&wp_fractional_scale_manager_v1_interface, 1);
    auto [s, ss] = b.surface();
    wp_alpha_modifier_surface_v1* a = wp_alpha_modifier_v1_get_surface(am, s);
    wp_alpha_modifier_surface_v1_set_multiplier(a, 0x7fffffff);
    b.pump();
    EXPECT_FLOAT_EQ(ss->current().alpha, 1);
    wl_surface_commit(s);
    b.pump();
    EXPECT_NEAR(ss->current().alpha, 0.5, 1e-6);
    wp_alpha_modifier_surface_v1_destroy(a);
    wl_surface_commit(s);
    b.pump();
    EXPECT_FLOAT_EQ(ss->current().alpha, 1);

    uint32_t scale = 0;
    wp_fractional_scale_v1* f = wp_fractional_scale_manager_v1_get_fractional_scale(fm, s);
    static const wp_fractional_scale_v1_listener fl = {
        .preferred_scale = [](void* d, wp_fractional_scale_v1*, uint32_t v) { *static_cast<uint32_t*>(d) = v; },
    };
    wp_fractional_scale_v1_add_listener(f, &fl, &scale);
    scales.set_preferred_scale(ss, 1.5);
    b.pump();
    EXPECT_EQ(scale, 180u);
    // A second object for the surface is an error.
    wp_fractional_scale_v1* second = wp_fractional_scale_manager_v1_get_fractional_scale(fm, s);
    b.pump();
    EXPECT_EQ(b.protocol_error(), uint32_t(WP_FRACTIONAL_SCALE_MANAGER_V1_ERROR_FRACTIONAL_SCALE_EXISTS));
    wp_fractional_scale_v1_destroy(second);
    wp_fractional_scale_v1_destroy(f);
    wp_fractional_scale_manager_v1_destroy(fm);
    wp_alpha_modifier_v1_destroy(am);
    wl_surface_destroy(s);
}

TEST(WlSurfaceExt, SinglePixelBuffers) {
    Buffers b;
    wl::SinglePixelBuffers pixels(b.server);
    auto* m = b.bind<wp_single_pixel_buffer_manager_v1>(&wp_single_pixel_buffer_manager_v1_interface, 1);
    wl_buffer* buf = wp_single_pixel_buffer_manager_v1_create_u32_rgba_buffer(m, 0xffffffff, 0, 0, 0xffffffff);
    auto [s, ss] = b.surface();
    wl_surface_attach(s, buf, 0, 0);
    wl_surface_commit(s);
    b.pump();
    float rgba[4];
    ASSERT_TRUE(wl::SinglePixelBuffers::color_of(ss->current().buffer.get(), rgba));
    EXPECT_FLOAT_EQ(rgba[0], 1);
    EXPECT_FLOAT_EQ(rgba[1], 0);
    EXPECT_EQ(ss->current().width, 1);
    wl_surface_destroy(s);
    wl_buffer_destroy(buf);
    wp_single_pixel_buffer_manager_v1_destroy(m);
    b.pump();
}

TEST(WlTiming, PresentationFeedbackPresentedOrDiscarded) {
    Buffers b;
    wl::Presentation presentation(b.server);
    auto* p = b.bind<wp_presentation>(&wp_presentation_interface, 2);
    auto* wo = b.bind<wl_output>(&wl_output_interface, 4);
    struct Log {
        int presented = 0, discarded = 0, synced = 0;
        uint32_t refresh = 0;
    } log;
    static const wp_presentation_feedback_listener fl = {
        .sync_output = [](void* d, struct wp_presentation_feedback*, wl_output*) { ++static_cast<Log*>(d)->synced; },
        .presented =
            [](void* d, struct wp_presentation_feedback* f, uint32_t, uint32_t, uint32_t, uint32_t refresh, uint32_t,
               uint32_t, uint32_t) {
                ++static_cast<Log*>(d)->presented;
                static_cast<Log*>(d)->refresh = refresh;
                wp_presentation_feedback_destroy(f);
            },
        .discarded =
            [](void* d, struct wp_presentation_feedback* f) {
                ++static_cast<Log*>(d)->discarded;
                wp_presentation_feedback_destroy(f);
            },
    };
    auto [s, ss] = b.surface();
    wp_presentation_feedback_add_listener(wp_presentation_feedback(p, s), &fl, &log);
    wl_surface_attach(s, b.buffer(10, 10), 0, 0);
    wl_surface_commit(s);
    // Superseded by a new buffer before shown: discarded.
    wp_presentation_feedback_add_listener(wp_presentation_feedback(p, s), &fl, &log);
    wl_surface_attach(s, b.buffer(10, 10), 0, 0);
    wl_surface_commit(s);
    b.pump();
    EXPECT_EQ(log.discarded, 1);
    timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    wl::Presentation::presented(ss, &b.output, now, 16666666, 1, wl::Presentation::Vsync);
    b.pump();
    EXPECT_EQ(log.presented, 1);
    EXPECT_EQ(log.synced, 1);
    EXPECT_EQ(log.refresh, 16666666u);
    wl_output_release(wo);
    wl_surface_destroy(s);
    wp_presentation_destroy(p);
    b.pump();
    EXPECT_EQ(b.error(), 0);
}

TEST(WlTiming, FifoWaitsForTheRefresh) {
    Buffers b;
    wl::Fifo fifo(b.server);
    auto* m = b.bind<wp_fifo_manager_v1>(&wp_fifo_manager_v1_interface, 1);
    auto [s, ss] = b.surface();
    wp_fifo_v1* f = wp_fifo_manager_v1_get_fifo(m, s);
    wl_buffer* one = b.buffer(10, 10);
    wl_buffer* two = b.buffer(20, 20);
    wl_surface_attach(s, one, 0, 0);
    wp_fifo_v1_set_barrier(f);
    wl_surface_commit(s);
    wl_surface_attach(s, two, 0, 0);
    wp_fifo_v1_wait_barrier(f);
    wl_surface_commit(s);
    b.pump();
    EXPECT_EQ(ss->current().width, 10);  // the second waits
    fifo.refreshed(ss);
    b.pump();
    EXPECT_EQ(ss->current().width, 20);
    wp_fifo_v1_destroy(f);
    wp_fifo_manager_v1_destroy(m);
    wl_surface_destroy(s);
    b.pump();
    EXPECT_EQ(b.error(), 0);
}

TEST(WlTiming, CommitTimingHoldsUntilItsTime) {
    Buffers b;
    wl::CommitTiming timing(b.server);
    auto* m = b.bind<wp_commit_timing_manager_v1>(&wp_commit_timing_manager_v1_interface, 1);
    auto [s, ss] = b.surface();
    wp_commit_timer_v1* t = wp_commit_timing_manager_v1_get_timer(m, s);
    timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    const int64_t target = int64_t(now.tv_sec) * 1000000000 + now.tv_nsec + 60 * 1000000;
    wp_commit_timer_v1_set_timestamp(t, uint32_t(uint64_t(target / 1000000000) >> 32),
                                     uint32_t(target / 1000000000), uint32_t(target % 1000000000));
    wl_surface_attach(s, b.buffer(10, 10), 0, 0);
    wl_surface_commit(s);
    b.pump();
    EXPECT_EQ(ss->current().width, 0);
    usleep(90 * 1000);
    b.pump();
    EXPECT_EQ(ss->current().width, 10);
    wp_commit_timer_v1_destroy(t);
    wp_commit_timing_manager_v1_destroy(m);
    wl_surface_destroy(s);
    b.pump();
}

TEST(WlTiming, SyncobjNeedsPointsWithADmabuf) {
    int drm = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
    uint64_t cap = 0;
    if (drm < 0 || drmGetCap(drm, DRM_CAP_SYNCOBJ_TIMELINE, &cap) != 0 || !cap) {
        if (drm >= 0)
            close(drm);
        GTEST_SKIP() << "no render node with timeline syncobjs";
    }
    uint32_t handle = 0;
    {
    Buffers b;
    wl::Syncobj sync(b.server, drm);
    auto* m = b.bind<wp_linux_drm_syncobj_manager_v1>(&wp_linux_drm_syncobj_manager_v1_interface, 1);
    ASSERT_EQ(drmSyncobjCreate(drm, 0, &handle), 0);
    int fd = -1;
    ASSERT_EQ(drmSyncobjHandleToFD(drm, handle, &fd), 0);
    wp_linux_drm_syncobj_timeline_v1* tl = wp_linux_drm_syncobj_manager_v1_import_timeline(m, fd);
    close(fd);
    auto [s, ss] = b.surface();
    wp_linux_drm_syncobj_surface_v1* so = wp_linux_drm_syncobj_manager_v1_get_surface(m, s);
    int applied = 0;
    wl::Connection counted = ss->events.commit.connect([&] { ++applied; });
    // Points with an shm buffer: explicit sync is for dmabufs only.
    wl_surface_attach(s, b.buffer(10, 10), 0, 0);
    wp_linux_drm_syncobj_surface_v1_set_acquire_point(so, tl, 0, 1);
    wp_linux_drm_syncobj_surface_v1_set_release_point(so, tl, 0, 2);
    wl_surface_commit(s);
    b.pump();
    EXPECT_EQ(b.protocol_error(), uint32_t(WP_LINUX_DRM_SYNCOBJ_SURFACE_V1_ERROR_UNSUPPORTED_BUFFER));
    EXPECT_EQ(applied, 0);  // rejected: never applied (and the client is gone)
    wp_linux_drm_syncobj_surface_v1_destroy(so);
    wp_linux_drm_syncobj_timeline_v1_destroy(tl);
    wp_linux_drm_syncobj_manager_v1_destroy(m);
    wl_surface_destroy(s);
    }
    drmSyncobjDestroy(drm, handle);
    close(drm);
}
