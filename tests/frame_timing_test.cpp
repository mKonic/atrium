#include "frame_timing.hpp"
#include "util/timeline.hpp"

#include <xf86drm.h>
#include <gtest/gtest.h>

#include <fcntl.h>
#include <time.h>
#include <unistd.h>

using namespace atrium;
using namespace atrium::frame_timing;

namespace {

constexpr int64_t ms = 1'000'000;

int64_t now_ns() {
    timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1'000'000'000LL + t.tv_nsec;
}

} // namespace

TEST(FrameTiming, JournalRisesAtOnceAndFallsSlowly) {
    RenderJournal j;
    EXPECT_EQ(j.estimate(), 0);
    j.add(2 * ms);
    EXPECT_EQ(j.estimate(), 2 * ms);
    j.add(6 * ms);
    EXPECT_EQ(j.estimate(), 6 * ms);  // a slow frame counts straight away
    j.add(1 * ms);
    EXPECT_GT(j.estimate(), 5 * ms);  // one cheap frame barely moves it
    for (int i = 0; i < 200; ++i)
        j.add(1 * ms);
    EXPECT_LT(j.estimate(), 1 * ms + ms / 10);  // a run of them does
    j.add(0);
    j.add(-5);
    EXPECT_GT(j.estimate(), 0);  // nonsense ignored
}

TEST(FrameTiming, MarginIsTheCostPlusSlackWithinBounds) {
    const int64_t period = 1'000'000'000 / 180;  // 5.56 ms
    EXPECT_EQ(margin(0, ms, ms, period), ms);                   // nothing measured: the slack
    EXPECT_EQ(margin(ms / 2, ms, ms, period), ms + ms / 2);
    EXPECT_EQ(margin(0, 0, ms, period), ms);                    // at least the minimum
    EXPECT_EQ(margin(10 * ms, ms, ms, period), period / 2);     // never more than half a refresh
    EXPECT_EQ(margin(0, ms, 4 * ms, 6 * ms), 3 * ms);           // a minimum past half: half
    EXPECT_EQ(margin(2 * ms, ms, ms, 0), 3 * ms);               // no refresh known: unbounded
}

TEST(FrameTiming, NoFenceNoTime) {
    EXPECT_EQ(fence_signalled_ns(-1), 0);
    const int fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
    EXPECT_EQ(fence_signalled_ns(fd), 0);  // not a sync_file
    close(fd);
}

TEST(FrameTiming, SignalledFenceTellsWhen) {
    const int drm = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
    uint64_t cap = 0;
    if (drm < 0 || drmGetCap(drm, DRM_CAP_SYNCOBJ_TIMELINE, &cap) != 0 || !cap) {
        if (drm >= 0)
            close(drm);
        GTEST_SKIP() << "no render node with timeline syncobjs";
    }
    Timeline* t = timeline_create(drm);
    ASSERT_NE(t, nullptr);
    ASSERT_TRUE(timeline_signal(t, 1));
    const int64_t after = now_ns();
    const int fd = timeline_export_sync_file(t, 1);
    ASSERT_GE(fd, 0);
    // Signalled from the CPU it's the kernel's stub fence, stamped at boot
    // (a GPU's fence has the real time): some time, not later than now. A
    // render "finished before it started" is what the journal ignores.
    const int64_t when = fence_signalled_ns(fd);
    EXPECT_GT(when, 0);
    EXPECT_LE(when, after);
    close(fd);
    timeline_unref(t);
    close(drm);
}
