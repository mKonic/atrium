#include "util/buffer.hpp"
#include "util/damage_ring.hpp"

#include <drm_fourcc.h>
#include <gtest/gtest.h>

#include <array>

using namespace atrium;

namespace {

struct TestBuffer {
    Buffer base;
    bool* destroyed;
    uint32_t pixel = 0;
};

const BufferImpl kImpl = {
    .destroy =
        [](Buffer* b) {
            auto* t = reinterpret_cast<TestBuffer*>(b);
            buffer_finish(b);
            *t->destroyed = true;
            delete t;
        },
    .get_dmabuf = nullptr,
    .get_shm = nullptr,
    .begin_data_ptr_access =
        [](Buffer* b, uint32_t, void** data, uint32_t* format, size_t* stride) {
            auto* t = reinterpret_cast<TestBuffer*>(b);
            *data = &t->pixel;
            *format = DRM_FORMAT_ARGB8888;
            *stride = 4;
            return true;
        },
    .end_data_ptr_access = [](Buffer*) {},
};

TestBuffer* make(bool* destroyed, int w = 1, int h = 1) {
    auto* t = new TestBuffer{{}, destroyed};
    buffer_init(&t->base, &kImpl, w, h);
    return t;
}

} // namespace

TEST(UtilBuffer, GoesOnceDroppedAndUnlocked) {
    bool gone = false;
    TestBuffer* t = make(&gone);
    buffer_lock(&t->base);
    buffer_drop(&t->base);
    EXPECT_FALSE(gone);  // still locked
    buffer_unlock(&t->base);
    EXPECT_TRUE(gone);
}

TEST(UtilBuffer, AddonsGoWithIt) {
    bool gone = false;
    TestBuffer* t = make(&gone);
    static bool addon_gone;
    addon_gone = false;
    static const AddonInterface iface = {"test", [](Addon* a) {
                                             addon_finish(a);
                                             addon_gone = true;
                                         }};
    Addon a;
    addon_init(&a, &t->base.addons, &gone, &iface);
    EXPECT_EQ(addon_find(&t->base.addons, &gone, &iface), &a);
    EXPECT_EQ(addon_find(&t->base.addons, &addon_gone, &iface), nullptr);  // another owner
    buffer_drop(&t->base);
    EXPECT_TRUE(addon_gone);
    EXPECT_TRUE(gone);
}

TEST(UtilBuffer, SinglePixelOpacity) {
    bool gone = false;
    TestBuffer* t = make(&gone);
    t->pixel = 0xFF102030;  // ARGB, alpha 0xFF
    EXPECT_TRUE(buffer_is_opaque(&t->base));
    t->pixel = 0x80102030;
    EXPECT_FALSE(buffer_is_opaque(&t->base));
    buffer_drop(&t->base);
}

TEST(UtilDamageRing, EachBufferGetsWhatItMissed) {
    bool ga = false, gb = false;
    TestBuffer* a = make(&ga, 100, 100);
    TestBuffer* b = make(&gb, 100, 100);
    DamageRing ring;
    damage_ring_init(&ring);
    pixman_region32_t d;
    pixman_region32_init(&d);
    damage_ring_rotate_buffer(&ring, &a->base, &d);  // new: everything
    EXPECT_EQ(pixman_region32_extents(&d)->x2, 100);
    damage_ring_rotate_buffer(&ring, &b->base, &d);
    const Box hit{10, 10, 5, 5};
    damage_ring_add_box(&ring, &hit);
    damage_ring_rotate_buffer(&ring, &a->base, &d);
    // `a` missed only the box (b's first frame was whole, but a drew then too).
    const pixman_box32_t* e = pixman_region32_extents(&d);
    EXPECT_EQ((std::array{e->x1, e->y1, e->x2, e->y2}), (std::array{10, 10, 15, 15}));
    damage_ring_rotate_buffer(&ring, &b->base, &d);
    e = pixman_region32_extents(&d);
    EXPECT_EQ((std::array{e->x1, e->y1, e->x2, e->y2}), (std::array{10, 10, 15, 15}));  // b missed it too
    buffer_drop(&a->base);  // its entry squashes into the ring
    EXPECT_TRUE(ga);
    pixman_region32_fini(&d);
    damage_ring_finish(&ring);
    buffer_drop(&b->base);
}
