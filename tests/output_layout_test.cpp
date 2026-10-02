// OutputLayout and Cursor: placement, the closest point, the pointer kept on
// the screens.
#include "cursor.hpp"
#include "output_layout.hpp"

#include "backend/backend.hpp"

#include <gtest/gtest.h>

using atrium::Cursor;
using atrium::OutputLayout;
namespace backend = atrium::backend;

namespace {

class FakeBackend final : public backend::Backend {
public:
    explicit FakeBackend(wl_event_loop* loop) : Backend(loop) {}
    bool start() override { return true; }
    uint32_t buffer_caps() const override { return 0; }
};

class FakeOutput final : public backend::Output {
public:
    FakeOutput(backend::Backend& b, int w, int h) : Output(b) {
        modes.push_back({w, h, 60000, true});
        backend::OutputState s;
        s.set_enabled(true);
        s.set_mode(&modes[0]);
        EXPECT_TRUE(commit_state(s));
    }
    ~FakeOutput() override { events.destroy.emit(); }
    void set_scale(float k) {
        backend::OutputState s;
        s.set_scale(k);
        EXPECT_TRUE(commit_state(s));
    }

protected:
    bool test(const backend::OutputState&) override { return true; }
    bool commit(const backend::OutputState&) override { return true; }
};

struct Fixture : ::testing::Test {
    wl_event_loop* loop = wl_event_loop_create();
    FakeBackend backend{loop};
    ~Fixture() override { wl_event_loop_destroy(loop); }
};

bool same(const wlr_box& a, const wlr_box& b) {
    return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
}

} // namespace

using OutputLayoutTest = Fixture;

// Automatic screens go right of the placed ones, level with the rightmost.
TEST_F(OutputLayoutTest, AutomaticScreensGoRightOfPlacedOnes) {
    FakeOutput a(backend, 1920, 1080), b(backend, 1280, 1024), c(backend, 800, 600);
    OutputLayout l;
    l.add_auto(&c);
    l.add(&a, 0, 200);
    l.add(&b, -1280, 0);
    EXPECT_TRUE(same(l.box(&c), {1920, 200, 800, 600}));
    EXPECT_TRUE(same(l.extents(), {-1280, 0, 1280 + 1920 + 800, 1280}));
    EXPECT_EQ(l.output_at(-1, 5), &b);
    EXPECT_EQ(l.output_at(1920, 200), &c);
    EXPECT_EQ(l.output_at(1919.5, 100), nullptr);  // above a: no screen
}

// A screen's box is its scaled size, and a new scale moves its neighbours.
TEST_F(OutputLayoutTest, AScaleChangeResizesAndReflows) {
    FakeOutput a(backend, 3840, 2160), b(backend, 1920, 1080);
    OutputLayout l;
    l.add_auto(&a);
    l.add_auto(&b);
    int changes = 0;
    auto conn = l.change.connect([&] { ++changes; });
    a.set_scale(2);
    EXPECT_TRUE(same(l.box(&a), {0, 0, 1920, 1080}));
    EXPECT_TRUE(same(l.box(&b), {1920, 0, 1920, 1080}));
    EXPECT_EQ(changes, 1);
}

TEST_F(OutputLayoutTest, AScreenThatGoesLeavesTheLayout) {
    OutputLayout l;
    FakeOutput a(backend, 1000, 1000);
    {
        FakeOutput b(backend, 500, 500);
        l.add_auto(&a);
        l.add_auto(&b);
        EXPECT_EQ(l.extents().width, 1500);
    }
    EXPECT_EQ(l.outputs(), std::vector<backend::Output*>{&a});
    EXPECT_EQ(l.extents().width, 1000);
}

TEST_F(OutputLayoutTest, TheClosestPointStaysInsideTheEdges) {
    FakeOutput a(backend, 100, 100);
    OutputLayout l;
    l.add(&a, 0, 0);
    double x, y;
    l.closest_point(nullptr, 250, -30, &x, &y);
    EXPECT_DOUBLE_EQ(x, 100 - 1 / 256.0);
    EXPECT_DOUBLE_EQ(y, 0);
    EXPECT_EQ(l.output_at(x, y), &a);
}

using CursorTest = Fixture;

// Moving off a screen's edge stops at it; across a shared edge carries on.
TEST_F(CursorTest, TheCursorStaysOnTheScreens) {
    FakeOutput a(backend, 1000, 800), b(backend, 600, 400);
    OutputLayout l;
    l.add_auto(&a);
    l.add_auto(&b);
    Cursor c(l, loop);
    c.warp_closest(990, 300);
    c.move(50, 0);
    EXPECT_DOUBLE_EQ(c.x, 1040);
    EXPECT_EQ(l.output_at(c.x, c.y), &b);
    c.warp_closest(1500, 300);
    c.move(0, 500);  // below b: kept on its bottom edge
    EXPECT_DOUBLE_EQ(c.x, 1500);
    EXPECT_DOUBLE_EQ(c.y, 400 - 1 / 256.0);
    EXPECT_FALSE(c.warp(1100, 600));
    EXPECT_DOUBLE_EQ(c.x, 1500);
}

// The screen under the pointer goes: it lands on the one left.
TEST_F(CursorTest, TheCursorMovesOffAScreenThatGoes) {
    FakeOutput a(backend, 1000, 800);
    OutputLayout l;
    l.add_auto(&a);
    Cursor c(l, loop);
    {
        FakeOutput b(backend, 600, 400);
        l.add_auto(&b);
        c.warp_closest(1300, 200);
        EXPECT_EQ(l.output_at(c.x, c.y), &b);
    }
    EXPECT_EQ(l.output_at(c.x, c.y), &a);
}
