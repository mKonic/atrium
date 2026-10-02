#include "util/timeline.hpp"

#include <xf86drm.h>
#include <gtest/gtest.h>

#include <fcntl.h>
#include <unistd.h>

using namespace atrium;

namespace {

class TimelineTest : public ::testing::Test {
protected:
    void SetUp() override {
        drm = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
        uint64_t cap = 0;
        if (drm < 0 || drmGetCap(drm, DRM_CAP_SYNCOBJ_TIMELINE, &cap) != 0 || !cap)
            GTEST_SKIP() << "no render node with timeline syncobjs";
        t = timeline_create(drm);
        ASSERT_NE(t, nullptr);
    }
    void TearDown() override {
        timeline_unref(t);
        if (drm >= 0)
            close(drm);
    }
    bool signalled(uint64_t point) {
        bool r = false;
        EXPECT_TRUE(timeline_check(t, point, DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE, &r));
        return r;
    }
    int drm = -1;
    Timeline* t = nullptr;
};

TEST_F(TimelineTest, PointsSignalInOrder) {
    EXPECT_FALSE(signalled(1));
    ASSERT_TRUE(timeline_signal(t, 2));
    EXPECT_TRUE(signalled(1));  // a later point covers the earlier
    EXPECT_TRUE(signalled(2));
    EXPECT_FALSE(signalled(3));
}

TEST_F(TimelineTest, SyncFileRoundTrip) {
    ASSERT_TRUE(timeline_signal(t, 1));
    const int sf = timeline_export_sync_file(t, 1);
    ASSERT_GE(sf, 0);
    ASSERT_TRUE(timeline_import_sync_file(t, 5, sf));
    close(sf);
    EXPECT_TRUE(signalled(5));
    EXPECT_LT(timeline_export_sync_file(t, 9), 0);  // not materialised
}

TEST_F(TimelineTest, ImportSharesTheSyncobj) {
    const int fd = timeline_export(t);
    ASSERT_GE(fd, 0);
    Timeline* other = timeline_import(drm, fd);
    close(fd);
    ASSERT_NE(other, nullptr);
    ASSERT_TRUE(timeline_signal(other, 3));
    EXPECT_TRUE(signalled(3));
    EXPECT_EQ(timeline_ref(other), other);
    timeline_unref(other);
    timeline_unref(other);
}

TEST_F(TimelineTest, WaiterRunsOnceSignalled) {
    wl_event_loop* loop = wl_event_loop_create();
    struct W {
        TimelineWaiter base;
        int ran = 0;
    } w;
    ASSERT_TRUE(timeline_waiter_init(&w.base, t, 4, 0, loop,
                                     [](TimelineWaiter* x) { ++reinterpret_cast<W*>(x)->ran; }));
    wl_event_loop_dispatch(loop, 0);
    EXPECT_EQ(w.ran, 0);
    ASSERT_TRUE(timeline_signal(t, 4));
    wl_event_loop_dispatch(loop, 1000);
    EXPECT_EQ(w.ran, 1);
    timeline_waiter_finish(&w.base);
    wl_event_loop_destroy(loop);
}

} // namespace
