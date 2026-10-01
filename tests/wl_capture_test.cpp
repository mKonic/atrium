// Screen and window capture (src/wl/capture) against a real libwayland client.
#include "wl/capture.hpp"
#include "wl/compositor.hpp"
#include "wl/output.hpp"
#include "wl/shm.hpp"
#include "wl_harness.hpp"

#include "ext-image-capture-source-v1-client-protocol.h"
#include "ext-image-copy-capture-v1-client-protocol.h"
#include "wlr-screencopy-unstable-v1-client-protocol.h"

#include <wayland-client-protocol.h>

#include <gtest/gtest.h>

#include <drm_fourcc.h>
#include <sys/mman.h>
#include <unistd.h>

using namespace atrium;

namespace {

struct Cap : wltest::Harness {
    wl::Compositor compositor{server, nullptr};
    wl::Shm shm{server, {}};
    wl::Seat seat{server, "seat0"};
    wl::Output output{server, wl::OutputInfo{.name = "DP-1"}};
    wl::ForeignToplevels toplevels{server};
    wl::Capture capture{server, seat, toplevels};
    wl_output* wout = nullptr;
    wl_shm* wshm = nullptr;
    std::vector<wl_buffer*> buffers;
    // What the "compositor" is asked to render, and answers.
    std::vector<wl::Capture::Target> copied;
    bool answer_ok = true;
    wl::Connection on_copy = capture.copy.connect([this](wl::Capture::Copy& c) {
        copied.push_back(c.target);
        wl::Capture::Result r;
        r.ok = answer_ok;
        r.when = {1, 500};
        r.damage = {{0, 0, 8, 8}};
        r.fail_reason = 1;
        c.done(r);
    });

    Cap() {
        capture.constraints = [this](const wl::Capture::Target& t) -> std::optional<wl::Capture::Constraints> {
            if (t.output != &output)
                return std::nullopt;
            return wl::Capture::Constraints{.width = 16, .height = 8, .shm_format = DRM_FORMAT_XRGB8888,
                                            .shm_stride = 64};
        };
        wout = bind<wl_output>(&wl_output_interface, 4);
        wshm = bind<struct wl_shm>(&wl_shm_interface, 2);
        pump();
    }
    ~Cap() {
        if (!client)
            return;
        for (wl_buffer* b : buffers)
            wl_buffer_destroy(b);
        wl_shm_release(wshm);
        wl_output_release(wout);
        pump();
    }
    wl_buffer* buffer(int w, int h) {
        int fd = memfd_create("cap", MFD_CLOEXEC);
        EXPECT_EQ(ftruncate(fd, w * h * 4), 0);
        wl_shm_pool* pool = wl_shm_create_pool(wshm, fd, w * h * 4);
        wl_buffer* b = wl_shm_pool_create_buffer(pool, 0, w, h, w * 4, WL_SHM_FORMAT_XRGB8888);
        wl_shm_pool_destroy(pool);
        close(fd);
        buffers.push_back(b);
        return b;
    }
};

struct FrameLog {
    uint32_t w = 0, h = 0, format = 0;
    int done = 0, ready = 0, failed = 0, damage = 0;
};

const zwlr_screencopy_frame_v1_listener kFrame = {
    .buffer =
        [](void* d, zwlr_screencopy_frame_v1*, uint32_t format, uint32_t w, uint32_t h, uint32_t) {
            auto* l = static_cast<FrameLog*>(d);
            l->format = format;
            l->w = w;
            l->h = h;
        },
    .flags = [](void*, zwlr_screencopy_frame_v1*, uint32_t) {},
    .ready = [](void* d, zwlr_screencopy_frame_v1*, uint32_t, uint32_t,
                uint32_t) { ++static_cast<FrameLog*>(d)->ready; },
    .failed = [](void* d, zwlr_screencopy_frame_v1*) { ++static_cast<FrameLog*>(d)->failed; },
    .damage = [](void* d, zwlr_screencopy_frame_v1*, uint32_t, uint32_t, uint32_t,
                 uint32_t) { ++static_cast<FrameLog*>(d)->damage; },
    .linux_dmabuf = [](void*, zwlr_screencopy_frame_v1*, uint32_t, uint32_t, uint32_t) {},
    .buffer_done = [](void* d, zwlr_screencopy_frame_v1*) { ++static_cast<FrameLog*>(d)->done; },
};

} // namespace

TEST(WlCapture, ScreencopyOffersThenCopies) {
    Cap c;
    auto* m = c.bind<zwlr_screencopy_manager_v1>(&zwlr_screencopy_manager_v1_interface, 3);
    zwlr_screencopy_frame_v1* f = zwlr_screencopy_manager_v1_capture_output(m, 1, c.wout);
    FrameLog log;
    zwlr_screencopy_frame_v1_add_listener(f, &kFrame, &log);
    c.pump();
    EXPECT_EQ(log.w, 16u);
    EXPECT_EQ(log.format, uint32_t(WL_SHM_FORMAT_XRGB8888));
    EXPECT_EQ(log.done, 1);
    zwlr_screencopy_frame_v1_copy_with_damage(f, c.buffer(16, 8));
    c.pump();
    ASSERT_EQ(c.copied.size(), 1u);
    EXPECT_TRUE(c.copied[0].cursor);
    EXPECT_EQ(log.ready, 1);
    EXPECT_EQ(log.damage, 1);
    zwlr_screencopy_frame_v1_destroy(f);

    // A buffer that isn't what was offered is an error.
    zwlr_screencopy_frame_v1* g = zwlr_screencopy_manager_v1_capture_output(m, 0, c.wout);
    zwlr_screencopy_frame_v1_add_listener(g, &kFrame, &log);
    zwlr_screencopy_frame_v1_copy(g, c.buffer(10, 10));
    c.pump();
    EXPECT_TRUE(c.posted("zwlr_screencopy_frame_v1", ZWLR_SCREENCOPY_FRAME_V1_ERROR_INVALID_BUFFER));
    zwlr_screencopy_frame_v1_destroy(g);
    zwlr_screencopy_manager_v1_destroy(m);
}

TEST(WlCapture, ImageCopySessionsFollowTheirSource) {
    Cap c;
    auto* sm = c.bind<ext_output_image_capture_source_manager_v1>(&ext_output_image_capture_source_manager_v1_interface, 1);
    auto* cm = c.bind<ext_image_copy_capture_manager_v1>(&ext_image_copy_capture_manager_v1_interface, 1);
    ext_image_capture_source_v1* src = ext_output_image_capture_source_manager_v1_create_source(sm, c.wout);
    ext_image_copy_capture_session_v1* s = ext_image_copy_capture_manager_v1_create_session(cm, src, 0);
    struct Log {
        uint32_t w = 0;
        int done = 0, stopped = 0, ready = 0, failed = 0;
        uint32_t reason = 99;
    } log;
    static const ext_image_copy_capture_session_v1_listener sl = {
        .buffer_size = [](void* d, ext_image_copy_capture_session_v1*, uint32_t w,
                          uint32_t) { static_cast<Log*>(d)->w = w; },
        .shm_format = [](void*, ext_image_copy_capture_session_v1*, uint32_t) {},
        .dmabuf_device = [](void*, ext_image_copy_capture_session_v1*, wl_array*) {},
        .dmabuf_format = [](void*, ext_image_copy_capture_session_v1*, uint32_t, wl_array*) {},
        .done = [](void* d, ext_image_copy_capture_session_v1*) { ++static_cast<Log*>(d)->done; },
        .stopped = [](void* d, ext_image_copy_capture_session_v1*) { ++static_cast<Log*>(d)->stopped; },
    };
    static const ext_image_copy_capture_frame_v1_listener fl = {
        .transform = [](void*, ext_image_copy_capture_frame_v1*, uint32_t) {},
        .damage = [](void*, ext_image_copy_capture_frame_v1*, int32_t, int32_t, int32_t, int32_t) {},
        .presentation_time = [](void*, ext_image_copy_capture_frame_v1*, uint32_t, uint32_t, uint32_t) {},
        .ready = [](void* d, ext_image_copy_capture_frame_v1*) { ++static_cast<Log*>(d)->ready; },
        .failed =
            [](void* d, ext_image_copy_capture_frame_v1*, uint32_t r) {
                ++static_cast<Log*>(d)->failed;
                static_cast<Log*>(d)->reason = r;
            },
    };
    ext_image_copy_capture_session_v1_add_listener(s, &sl, &log);
    c.pump();
    EXPECT_EQ(log.w, 16u);
    EXPECT_EQ(log.done, 1);

    ext_image_copy_capture_frame_v1* f = ext_image_copy_capture_session_v1_create_frame(s);
    ext_image_copy_capture_frame_v1_add_listener(f, &fl, &log);
    ext_image_copy_capture_frame_v1_attach_buffer(f, c.buffer(16, 8));
    ext_image_copy_capture_frame_v1_capture(f);
    c.pump();
    EXPECT_EQ(log.ready, 1);
    ext_image_copy_capture_frame_v1_destroy(f);

    // A buffer the wrong size fails with that reason, without an error.
    f = ext_image_copy_capture_session_v1_create_frame(s);
    ext_image_copy_capture_frame_v1_add_listener(f, &fl, &log);
    ext_image_copy_capture_frame_v1_attach_buffer(f, c.buffer(4, 4));
    ext_image_copy_capture_frame_v1_capture(f);
    c.pump();
    EXPECT_EQ(log.failed, 1);
    EXPECT_EQ(log.reason, uint32_t(EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_BUFFER_CONSTRAINTS));
    ext_image_copy_capture_frame_v1_destroy(f);

    // The screen goes: the session stops.
    c.capture.stop({&c.output});
    c.pump();
    EXPECT_EQ(log.stopped, 1);
    ext_image_copy_capture_session_v1_destroy(s);
    ext_image_capture_source_v1_destroy(src);
    ext_image_copy_capture_manager_v1_destroy(cm);
    ext_output_image_capture_source_manager_v1_destroy(sm);
    c.pump();
    EXPECT_EQ(c.error(), 0);
}
