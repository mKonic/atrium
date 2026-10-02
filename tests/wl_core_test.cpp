// The protocol layer's core (src/wl: compositor, shm, output) against a real
// libwayland client.
#include "wl/compositor.hpp"
#include "wl/output.hpp"
#include "wl/shm.hpp"
#include "wl_harness.hpp"

#include "xdg-output-unstable-v1-client-protocol.h"

#include <wayland-client-protocol.h>

#include <gtest/gtest.h>

#include <drm_fourcc.h>
#include <sys/mman.h>
#include <unistd.h>

#include <memory>

using namespace atrium;

namespace {

struct Core : wltest::Harness {
    wl::Compositor compositor{server, nullptr};
    wl::Shm shm{server, {}};
    std::vector<wl::Surface*> surfaces;  // server side, as made
    wl::Signal<wl::Surface*>::Connection made =
        compositor.new_surface.connect([this](wl::Surface* s) { surfaces.push_back(s); });

    wl_compositor* wl_comp = nullptr;
    wl_subcompositor* wl_sub = nullptr;
    wl_shm* wl_shm_ = nullptr;

    Core() {
        wl_comp = bind<wl_compositor>(&wl_compositor_interface);
        wl_sub = bind<wl_subcompositor>(&wl_subcompositor_interface);
        wl_shm_ = bind<struct wl_shm>(&wl_shm_interface, 2);
        pump();
    }
    ~Core() {
        if (client) {
            wl_compositor_destroy(wl_comp);
            wl_subcompositor_destroy(wl_sub);
            wl_shm_release(wl_shm_);
        }
    }

    // A width x height ARGB buffer in a fresh pool (memfd `fd` kept open).
    wl_buffer* buffer(int width, int height, int* fd_out = nullptr) {
        const int stride = width * 4, size = stride * height;
        int fd = memfd_create("wl-core-test", MFD_CLOEXEC);
        EXPECT_EQ(ftruncate(fd, size), 0);
        wl_shm_pool* pool = wl_shm_create_pool(wl_shm_, fd, size);
        wl_buffer* b = wl_shm_pool_create_buffer(pool, 0, width, height, stride, WL_SHM_FORMAT_ARGB8888);
        wl_shm_pool_destroy(pool);
        if (fd_out)
            *fd_out = fd;
        else
            close(fd);
        return b;
    }

    wl::Surface* server_surface(wl_surface*) {
        pump();
        return surfaces.empty() ? nullptr : surfaces.back();
    }
};

} // namespace

TEST(WlCore, CommitAppliesPendingStateOnly) {
    Core c;
    wl_surface* s = wl_compositor_create_surface(c.wl_comp);
    wl::Surface* ss = c.server_surface(s);
    ASSERT_NE(ss, nullptr);
    wl_buffer* b = c.buffer(100, 60);
    wl_surface_attach(s, b, 0, 0);
    wl_surface_set_buffer_scale(s, 2);
    wl_surface_damage_buffer(s, 10, 10, 5, 5);
    c.pump();
    // Nothing applies before the commit.
    EXPECT_EQ(ss->current().buffer_width, 0);
    EXPECT_EQ(ss->current().scale, 1);

    int commits = 0;
    auto conn = ss->events.commit.connect([&] { ++commits; });
    wl_surface_commit(s);
    c.pump();
    EXPECT_EQ(commits, 1);
    EXPECT_EQ(ss->current().buffer_width, 100);
    EXPECT_EQ(ss->current().width, 50);
    EXPECT_EQ(ss->current().height, 30);
    ASSERT_NE(ss->buffer(), nullptr);
    // A new buffer of a new size is damaged whole.
    pixman_box32_t e = ss->buffer_damage().extents();
    EXPECT_EQ(e.x2 - e.x1, 100);

    // The next commit carries only its own damage, scaled into the buffer.
    wl_surface_damage(s, 1, 1, 2, 2);
    wl_surface_commit(s);
    c.pump();
    e = ss->buffer_damage().extents();
    EXPECT_EQ(e.x1, 2);
    EXPECT_EQ(e.x2, 6);
    EXPECT_EQ(ss->current().width, 50);  // still the buffer from before
    EXPECT_EQ(commits, 2);

    wl_surface_destroy(s);
    wl_buffer_destroy(b);
    c.pump();
    EXPECT_EQ(c.error(), 0);
}

TEST(WlCore, BufferNotAMultipleOfItsScaleIsAnError) {
    Core c;
    wl_surface* s = wl_compositor_create_surface(c.wl_comp);
    wl_buffer* b = c.buffer(101, 60);
    wl_surface_attach(s, b, 0, 0);
    wl_surface_set_buffer_scale(s, 2);
    wl_surface_commit(s);
    c.pump();
    EXPECT_EQ(c.error(), EPROTO);
    EXPECT_TRUE(c.posted("wl_surface", WL_SURFACE_ERROR_INVALID_SIZE));
    wl_surface_destroy(s);
    wl_buffer_destroy(b);
}

TEST(WlCore, ReplacedBufferIsReleased) {
    Core c;
    wl_surface* s = wl_compositor_create_surface(c.wl_comp);
    c.pump();
    int released = 0;
    static const wl_buffer_listener bl = {.release = [](void* d, wl_buffer*) { ++*static_cast<int*>(d); }};
    wl_buffer* a = c.buffer(10, 10);
    wl_buffer* b = c.buffer(10, 10);
    wl_buffer_add_listener(a, &bl, &released);
    wl_surface_attach(s, a, 0, 0);
    wl_surface_commit(s);
    c.pump();
    EXPECT_EQ(released, 0);  // shown (no renderer here to copy it out)
    wl_surface_attach(s, b, 0, 0);
    wl_surface_commit(s);
    c.pump();
    EXPECT_EQ(released, 1);
    wl_surface_destroy(s);
    wl_buffer_destroy(a);
    wl_buffer_destroy(b);
    c.pump();
}

TEST(WlCore, FrameCallbacksFireOnce) {
    Core c;
    wl_surface* s = wl_compositor_create_surface(c.wl_comp);
    wl::Surface* ss = c.server_surface(s);
    uint32_t done_at = 0;
    int dones = 0;
    static const wl_callback_listener cl = {.done = [](void* d, wl_callback* cb, uint32_t ms) {
        auto* p = static_cast<std::pair<uint32_t*, int*>*>(d);
        *p->first = ms;
        ++*p->second;
        wl_callback_destroy(cb);
    }};
    std::pair<uint32_t*, int*> data{&done_at, &dones};
    wl_callback_add_listener(wl_surface_frame(s), &cl, &data);
    c.pump();
    ss->send_frame_done(5);
    c.pump();
    EXPECT_EQ(dones, 0);  // not committed yet
    wl_surface_commit(s);
    c.pump();
    EXPECT_TRUE(ss->wants_frame());
    ss->send_frame_done(1234);
    ss->send_frame_done(1235);
    c.pump();
    EXPECT_EQ(dones, 1);
    EXPECT_EQ(done_at, 1234u);
    EXPECT_FALSE(ss->wants_frame());
    wl_surface_destroy(s);
    c.pump();
    EXPECT_EQ(c.error(), 0);
}

TEST(WlCore, InputRegion) {
    Core c;
    wl_surface* s = wl_compositor_create_surface(c.wl_comp);
    wl::Surface* ss = c.server_surface(s);
    wl_buffer* b = c.buffer(100, 100);
    wl_surface_attach(s, b, 0, 0);
    wl_surface_commit(s);
    c.pump();
    EXPECT_TRUE(ss->accepts_input(99, 99));
    EXPECT_FALSE(ss->accepts_input(100, 50));  // outside the surface

    wl_region* r = wl_compositor_create_region(c.wl_comp);
    wl_region_add(r, 0, 0, 50, 50);
    wl_region_subtract(r, 0, 0, 10, 10);
    wl_surface_set_input_region(s, r);
    wl_region_destroy(r);  // the surface keeps a copy
    wl_surface_commit(s);
    c.pump();
    EXPECT_TRUE(ss->accepts_input(20, 20));
    EXPECT_FALSE(ss->accepts_input(5, 5));
    EXPECT_FALSE(ss->accepts_input(60, 60));
    wl_surface_destroy(s);
    wl_buffer_destroy(b);
    c.pump();
}

namespace {

// A parent with a buffer, mapped by the test (no shell here), and a child.
struct Tree {
    Core& c;
    wl_surface *parent, *child;
    wl_subsurface* sub;
    wl::Surface *sparent, *schild;
    wl_buffer *pb, *cb;

    explicit Tree(Core& core) : c(core) {
        parent = wl_compositor_create_surface(c.wl_comp);
        sparent = c.server_surface(parent);
        child = wl_compositor_create_surface(c.wl_comp);
        schild = c.server_surface(child);
        sub = wl_subcompositor_get_subsurface(c.wl_sub, child, parent);
        pb = c.buffer(100, 100);
        cb = c.buffer(20, 20);
        wl_surface_attach(parent, pb, 0, 0);
        wl_surface_commit(parent);
        c.pump();
        sparent->map();
    }
    ~Tree() {
        if (!c.client)
            return;
        wl_subsurface_destroy(sub);
        wl_surface_destroy(child);
        wl_surface_destroy(parent);
        wl_buffer_destroy(pb);
        wl_buffer_destroy(cb);
        c.pump();
    }
};

} // namespace

TEST(WlCore, SynchronizedSubsurfaceWaitsForItsParent) {
    Core c;
    Tree t(c);
    ASSERT_NE(t.schild->subsurface(), nullptr);
    wl_surface_attach(t.child, t.cb, 0, 0);
    wl_surface_commit(t.child);
    c.pump();
    EXPECT_EQ(t.schild->current().buffer_width, 0);  // cached until the parent commits
    EXPECT_FALSE(t.schild->mapped());
    wl_subsurface_set_position(t.sub, 7, 9);
    wl_surface_commit(t.parent);
    c.pump();
    EXPECT_EQ(t.schild->current().buffer_width, 20);
    EXPECT_TRUE(t.schild->mapped());
    EXPECT_EQ(t.schild->subsurface()->x(), 7);
    EXPECT_EQ(t.schild->subsurface()->y(), 9);
    EXPECT_EQ(c.error(), 0);
}

TEST(WlCore, DesynchronizedSubsurfaceAppliesAtOnce) {
    Core c;
    Tree t(c);
    wl_subsurface_set_desync(t.sub);
    wl_surface_commit(t.parent);  // takes the child into its order
    wl_surface_attach(t.child, t.cb, 0, 0);
    wl_surface_commit(t.child);
    c.pump();
    EXPECT_EQ(t.schild->current().buffer_width, 20);
    EXPECT_TRUE(t.schild->mapped());
    // Unmapping the parent takes the child with it.
    t.sparent->unmap();
    EXPECT_FALSE(t.schild->mapped());
}

TEST(WlCore, SubsurfaceStacking) {
    Core c;
    Tree t(c);
    wl_surface* other = wl_compositor_create_surface(c.wl_comp);
    wl_subsurface* osub = wl_subcompositor_get_subsurface(c.wl_sub, other, t.parent);
    wl_surface_commit(t.parent);
    c.pump();
    auto order = [&] {
        std::vector<std::string> out;
        for (const auto& p : t.sparent->children())
            out.push_back(!p.sub ? "parent" : p.sub->surface() == t.schild ? "child" : "other");
        return out;
    };
    EXPECT_EQ(order(), (std::vector<std::string>{"parent", "child", "other"}));
    wl_subsurface_place_below(t.sub, t.parent);
    c.pump();
    EXPECT_EQ(order(), (std::vector<std::string>{"parent", "child", "other"}));  // pending until commit
    wl_surface_commit(t.parent);
    c.pump();
    EXPECT_EQ(order(), (std::vector<std::string>{"child", "parent", "other"}));
    wl_subsurface_place_above(t.sub, other);
    wl_surface_commit(t.parent);
    c.pump();
    EXPECT_EQ(order(), (std::vector<std::string>{"parent", "other", "child"}));
    wl_subsurface_destroy(osub);
    c.pump();
    EXPECT_EQ(order(), (std::vector<std::string>{"parent", "child"}));
    wl_surface_destroy(other);
    c.pump();
    EXPECT_EQ(c.error(), 0);
}

TEST(WlCore, SubsurfaceRoleErrors) {
    {
        Core c;
        wl_surface* a = wl_compositor_create_surface(c.wl_comp);
        wl_surface* b = wl_compositor_create_surface(c.wl_comp);
        wl_subsurface* s1 = wl_subcompositor_get_subsurface(c.wl_sub, b, a);
        wl_subsurface* s2 = wl_subcompositor_get_subsurface(c.wl_sub, a, b);  // a loop
        c.pump();
        EXPECT_TRUE(c.posted("wl_subcompositor", WL_SUBCOMPOSITOR_ERROR_BAD_PARENT));
        for (wl_subsurface* x : {s1, s2})
            wl_subsurface_destroy(x);
        wl_surface_destroy(a);
        wl_surface_destroy(b);
    }
    {
        Core c;
        wl_surface* a = wl_compositor_create_surface(c.wl_comp);
        wl_surface* b = wl_compositor_create_surface(c.wl_comp);
        wl_subsurface* s1 = wl_subcompositor_get_subsurface(c.wl_sub, b, a);
        wl_subsurface* s2 = wl_subcompositor_get_subsurface(c.wl_sub, b, a);  // already has the role
        c.pump();
        EXPECT_TRUE(c.posted("wl_subcompositor", WL_SUBCOMPOSITOR_ERROR_BAD_SURFACE));
        for (wl_subsurface* x : {s1, s2})
            wl_subsurface_destroy(x);
        wl_surface_destroy(a);
        wl_surface_destroy(b);
    }
}

TEST(WlCore, SurfaceGoneBeforeItsSubsurface) {
    Core c;
    wl_surface* parent = wl_compositor_create_surface(c.wl_comp);
    wl_surface* child = wl_compositor_create_surface(c.wl_comp);
    wl_subsurface* sub = wl_subcompositor_get_subsurface(c.wl_sub, child, parent);
    c.pump();
    wl::Surface* sparent = c.surfaces[0];
    wl_surface_destroy(child);
    wl_surface_commit(parent);
    c.pump();
    EXPECT_TRUE(sparent->children().size() == 1 && !sparent->children()[0].sub);
    // The subsurface went inert; the client's destroy is still fine.
    wl_subsurface_set_position(sub, 1, 1);
    wl_subsurface_destroy(sub);
    wl_surface_destroy(parent);
    c.pump();
    EXPECT_EQ(c.error(), 0);
}

TEST(WlCore, ShrunkPoolDoesNotCrashTheServer) {
    Core c;
    wl_surface* s = wl_compositor_create_surface(c.wl_comp);
    wl::Surface* ss = c.server_surface(s);
    int fd = -1;
    wl_buffer* b = c.buffer(64, 64, &fd);
    wl_surface_attach(s, b, 0, 0);
    wl_surface_commit(s);
    c.pump();
    ASSERT_NE(ss->current().buffer.get(), nullptr);
    ASSERT_EQ(ftruncate(fd, 0), 0);
    void* data;
    uint32_t format;
    size_t stride;
    Buffer* wb = ss->current().buffer.get();
    ASSERT_TRUE(buffer_begin_data_ptr_access(wb, BUFFER_DATA_PTR_ACCESS_READ, &data, &format, &stride));
    EXPECT_EQ(static_cast<volatile uint8_t*>(data)[4096 * 3], 0);  // would be SIGBUS
    buffer_end_data_ptr_access(wb);
    c.pump();
    EXPECT_EQ(c.error(), EPROTO);  // the client hears about it
    close(fd);
    wl_surface_destroy(s);
    wl_buffer_destroy(b);
}

// Screen capture writes into a client's shm buffer; what it writes is what
// the client reads.
TEST(WlCore, ShmBuffersCanBeWrittenForCapture) {
    Core c;
    wl_surface* s = wl_compositor_create_surface(c.wl_comp);
    wl::Surface* ss = c.server_surface(s);
    int fd = -1;
    wl_buffer* b = c.buffer(16, 16, &fd);
    wl_surface_attach(s, b, 0, 0);
    wl_surface_commit(s);
    c.pump();
    Buffer* wb = ss->current().buffer.get();
    ASSERT_NE(wb, nullptr);
    void* data;
    uint32_t format;
    size_t stride;
    ASSERT_TRUE(buffer_begin_data_ptr_access(wb, BUFFER_DATA_PTR_ACCESS_WRITE, &data, &format, &stride));
    static_cast<uint8_t*>(data)[5] = 0xab;
    buffer_end_data_ptr_access(wb);
    uint8_t seen = 0;
    ASSERT_EQ(pread(fd, &seen, 1, 5), 1);
    EXPECT_EQ(seen, 0xab);
    close(fd);
    wl_surface_destroy(s);
    wl_buffer_destroy(b);
    c.pump();
    EXPECT_EQ(c.error(), 0);
}

TEST(WlCore, OutputsDescribeAndUpdate) {
    Core c;
    wl::OutputInfo info{.name = "DP-1", .description = "Test screen", .make = "ACME", .model = "M1",
                        .scale = 1.5, .mode_width = 1920, .mode_height = 1080, .refresh = 180000,
                        .x = 0, .y = 0, .logical_width = 1280, .logical_height = 720};
    wl::Output out(c.server, info);
    wl::XdgOutputs xdg(c.server);

    struct Seen {
        std::string name, description;
        int w = 0, h = 0, scale = 0, dones = 0;
        int lw = 0, lh = 0;
    } seen;
    static const wl_output_listener ol = {
        .geometry = [](void*, wl_output*, int32_t, int32_t, int32_t, int32_t, int32_t, const char*, const char*,
                       int32_t) {},
        .mode =
            [](void* d, wl_output*, uint32_t, int32_t w, int32_t h, int32_t) {
                static_cast<Seen*>(d)->w = w;
                static_cast<Seen*>(d)->h = h;
            },
        .done = [](void* d, wl_output*) { ++static_cast<Seen*>(d)->dones; },
        .scale = [](void* d, wl_output*, int32_t f) { static_cast<Seen*>(d)->scale = f; },
        .name = [](void* d, wl_output*, const char* n) { static_cast<Seen*>(d)->name = n; },
        .description = [](void* d, wl_output*, const char* n) { static_cast<Seen*>(d)->description = n; },
    };
    auto* o = c.bind<wl_output>(&wl_output_interface, 4);
    ASSERT_NE(o, nullptr);
    wl_output_add_listener(o, &ol, &seen);
    c.pump();
    EXPECT_EQ(seen.name, "DP-1");
    EXPECT_EQ(seen.w, 1920);
    EXPECT_EQ(seen.scale, 2);  // 1.5, rounded up
    EXPECT_EQ(seen.dones, 1);

    auto* m = c.bind<zxdg_output_manager_v1>(&zxdg_output_manager_v1_interface, 3);
    zxdg_output_v1* xo = zxdg_output_manager_v1_get_xdg_output(m, o);
    static const zxdg_output_v1_listener xl = {
        .logical_position = [](void*, zxdg_output_v1*, int32_t, int32_t) {},
        .logical_size =
            [](void* d, zxdg_output_v1*, int32_t w, int32_t h) {
                static_cast<Seen*>(d)->lw = w;
                static_cast<Seen*>(d)->lh = h;
            },
        .done = [](void*, zxdg_output_v1*) {},
        .name = [](void*, zxdg_output_v1*, const char*) {},
        .description = [](void*, zxdg_output_v1*, const char*) {},
    };
    zxdg_output_v1_add_listener(xo, &xl, &seen);
    c.pump();
    EXPECT_EQ(seen.lw, 1280);
    EXPECT_EQ(seen.dones, 2);  // v3: wl_output.done closes the xdg-output batch

    info.mode_width = 2560;
    info.logical_width = 1706;
    info.description = "Renamed";
    out.update(info);
    c.pump();
    EXPECT_EQ(seen.w, 2560);
    EXPECT_EQ(seen.lw, 1706);
    EXPECT_EQ(seen.description, "Renamed");
    EXPECT_EQ(seen.dones, 3);
    out.update(info);  // no change: nothing sent
    c.pump();
    EXPECT_EQ(seen.dones, 3);

    // A surface on it hears so.
    wl_surface* s = wl_compositor_create_surface(c.wl_comp);
    wl::Surface* ss = c.server_surface(s);
    int entered = 0;
    static const wl_surface_listener sl = {
        .enter = [](void* d, wl_surface*, wl_output*) { ++*static_cast<int*>(d); },
        .leave = [](void* d, wl_surface*, wl_output*) { --*static_cast<int*>(d); },
        .preferred_buffer_scale = [](void*, wl_surface*, int32_t) {},
        .preferred_buffer_transform = [](void*, wl_surface*, uint32_t) {},
    };
    wl_surface_add_listener(s, &sl, &entered);
    ss->enter(out);
    ss->enter(out);
    c.pump();
    EXPECT_EQ(entered, 1);
    ss->leave(out);
    c.pump();
    EXPECT_EQ(entered, 0);

    zxdg_output_v1_destroy(xo);
    zxdg_output_manager_v1_destroy(m);
    wl_surface_destroy(s);
    wl_output_release(o);
    c.pump();
    EXPECT_EQ(c.error(), 0);
}

TEST(WlCore, OutputRemovedWhileBound) {
    Core c;
    auto out = std::make_unique<wl::Output>(c.server, wl::OutputInfo{.name = "HDMI-A-1"});
    auto* o = c.bind<wl_output>(&wl_output_interface, 4);
    c.pump();
    out.reset();
    c.pump();
    // The client's object is inert, not an error.
    wl_output_release(o);
    c.pump();
    EXPECT_EQ(c.error(), 0);
}
