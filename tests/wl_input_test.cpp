// Input-side protocols (src/wl/input_ext) against a real libwayland client.
#include "wl/compositor.hpp"
#include "wl/input_ext.hpp"
#include "wl/seat.hpp"
#include "wl_harness.hpp"

#include "cursor-shape-v1-client-protocol.h"
#include "ext-idle-notify-v1-client-protocol.h"
#include "idle-inhibit-unstable-v1-client-protocol.h"
#include "keyboard-shortcuts-inhibit-unstable-v1-client-protocol.h"
#include "pointer-constraints-unstable-v1-client-protocol.h"
#include "pointer-gestures-unstable-v1-client-protocol.h"
#include "relative-pointer-unstable-v1-client-protocol.h"

#include <wayland-client-protocol.h>

#include <gtest/gtest.h>

#include <unistd.h>

using namespace atrium;

namespace {

struct Input : wltest::Harness {
    wl::Compositor compositor{server, nullptr};
    wl::Seat seat{server, "seat0"};
    std::vector<wl::Surface*> surfaces;
    wl::Connection made = compositor.new_surface.connect([this](wl::Surface* s) { surfaces.push_back(s); });
    wl_compositor* comp = nullptr;
    wl_seat* wseat = nullptr;
    wl_pointer* pointer = nullptr;

    Input() {
        seat.set_capabilities(wl::Seat::Pointer | wl::Seat::Keyboard);
        comp = bind<wl_compositor>(&wl_compositor_interface);
        wseat = bind<wl_seat>(&wl_seat_interface);
        pump();
        pointer = wl_seat_get_pointer(wseat);
        pump();
    }
    ~Input() {
        if (!client)
            return;
        wl_pointer_release(pointer);
        wl_seat_release(wseat);
        wl_compositor_destroy(comp);
        pump();
    }
    std::pair<wl_surface*, wl::Surface*> surface() {
        wl_surface* s = wl_compositor_create_surface(comp);
        pump();
        return {s, surfaces.back()};
    }
};

} // namespace

TEST(WlInput, RelativeMotionGoesToTheFocusedClient) {
    Input in;
    wl::RelativePointers rel(in.server, in.seat);
    auto* m = in.bind<zwp_relative_pointer_manager_v1>(&zwp_relative_pointer_manager_v1_interface, 1);
    zwp_relative_pointer_v1* r = zwp_relative_pointer_manager_v1_get_relative_pointer(m, in.pointer);
    double got = 0;
    static const zwp_relative_pointer_v1_listener rl = {
        .relative_motion = [](void* d, zwp_relative_pointer_v1*, uint32_t, uint32_t, wl_fixed_t dx, wl_fixed_t,
                              wl_fixed_t, wl_fixed_t) { *static_cast<double*>(d) += wl_fixed_to_double(dx); },
    };
    zwp_relative_pointer_v1_add_listener(r, &rl, &got);
    auto [s, ss] = in.surface();
    rel.send_motion(1, 5, 0, 5, 0);  // not focused: nothing
    in.pump();
    EXPECT_EQ(got, 0);
    in.seat.pointer_enter(ss, 1, 1);
    int frames = 0;
    static const wl_pointer_listener pl = {
        .enter = [](void*, wl_pointer*, uint32_t, wl_surface*, wl_fixed_t, wl_fixed_t) {},
        .leave = [](void*, wl_pointer*, uint32_t, wl_surface*) {},
        .motion = [](void*, wl_pointer*, uint32_t, wl_fixed_t, wl_fixed_t) {},
        .button = [](void*, wl_pointer*, uint32_t, uint32_t, uint32_t, uint32_t) {},
        .axis = [](void*, wl_pointer*, uint32_t, uint32_t, wl_fixed_t) {},
        .frame = [](void* d, wl_pointer*) { ++*static_cast<int*>(d); },
        .axis_source = [](void*, wl_pointer*, uint32_t) {},
        .axis_stop = [](void*, wl_pointer*, uint32_t, uint32_t) {},
        .axis_discrete = [](void*, wl_pointer*, uint32_t, int32_t) {},
        .axis_value120 = [](void*, wl_pointer*, uint32_t, int32_t) {},
        .axis_relative_direction = [](void*, wl_pointer*, uint32_t, uint32_t) {},
    };
    wl_pointer_add_listener(in.pointer, &pl, &frames);
    in.seat.pointer_frame();
    in.pump();
    const int after_enter = frames;
    rel.send_motion(2, 3.5, 0, 3.5, 0);
    in.seat.pointer_frame();  // the input's frame, as libinput's comes
    in.pump();
    EXPECT_DOUBLE_EQ(got, 3.5);
    // Its own frame, with no absolute motion (a locked pointer): Xwayland
    // applies relative motion only on one.
    EXPECT_EQ(frames, after_enter + 1);
    zwp_relative_pointer_v1_destroy(r);
    zwp_relative_pointer_manager_v1_destroy(m);
    wl_surface_destroy(s);
}

TEST(WlInput, OneShotLockIsSpentOnceReleased) {
    Input in;
    wl::PointerConstraints pc(in.server, in.seat);
    wl::PointerConstraints::Constraint* made = nullptr;
    auto c = pc.events.new_constraint.connect([&](auto* x) { made = x; });
    auto* m = in.bind<zwp_pointer_constraints_v1>(&zwp_pointer_constraints_v1_interface, 1);
    auto [s, ss] = in.surface();
    zwp_locked_pointer_v1* lock = zwp_pointer_constraints_v1_lock_pointer(m, s, in.pointer, nullptr,
                                                                          ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_ONESHOT);
    int locked = 0, unlocked = 0;
    static const zwp_locked_pointer_v1_listener ll = {
        .locked = [](void* d, zwp_locked_pointer_v1*) { ++static_cast<int*>(d)[0]; },
        .unlocked = [](void* d, zwp_locked_pointer_v1*) { ++static_cast<int*>(d)[1]; },
    };
    int counts[2] = {0, 0};
    zwp_locked_pointer_v1_add_listener(lock, &ll, counts);
    in.pump();
    ASSERT_NE(made, nullptr);
    EXPECT_EQ(pc.for_surface(ss), made);
    pc.activate(made);
    pc.deactivate(made);
    pc.activate(made);  // spent: no second lock
    in.pump();
    locked = counts[0];
    unlocked = counts[1];
    EXPECT_EQ(locked, 1);
    EXPECT_EQ(unlocked, 1);

    // The region applies with the surface's commit.
    wl_region* r = wl_compositor_create_region(in.comp);
    wl_region_add(r, 0, 0, 10, 10);
    zwp_locked_pointer_v1_set_region(lock, r);
    wl_region_destroy(r);
    in.pump();
    EXPECT_TRUE(made->region.contains(50, 50));
    wl_surface_commit(s);
    in.pump();
    EXPECT_FALSE(made->region.contains(50, 50));

    // A second constraint on the surface is an error.
    zwp_confined_pointer_v1* again = zwp_pointer_constraints_v1_confine_pointer(
        m, s, in.pointer, nullptr, ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT);
    in.pump();
    EXPECT_TRUE(in.posted("zwp_pointer_constraints_v1", ZWP_POINTER_CONSTRAINTS_V1_ERROR_ALREADY_CONSTRAINED));
    zwp_confined_pointer_v1_destroy(again);
    zwp_locked_pointer_v1_destroy(lock);
    zwp_pointer_constraints_v1_destroy(m);
    wl_surface_destroy(s);
}

TEST(WlInput, GesturesFollowTheSurfaceTheyBeganOver) {
    Input in;
    wl::PointerGestures pg(in.server, in.seat);
    auto* m = in.bind<zwp_pointer_gestures_v1>(&zwp_pointer_gestures_v1_interface, 3);
    zwp_pointer_gesture_pinch_v1* pinch = zwp_pointer_gestures_v1_get_pinch_gesture(m, in.pointer);
    struct Log {
        int begins = 0, ends = 0;
        double scale = 0;
    } log;
    static const zwp_pointer_gesture_pinch_v1_listener pl = {
        .begin = [](void* d, zwp_pointer_gesture_pinch_v1*, uint32_t, uint32_t, wl_surface*,
                    uint32_t) { ++static_cast<Log*>(d)->begins; },
        .update = [](void* d, zwp_pointer_gesture_pinch_v1*, uint32_t, wl_fixed_t, wl_fixed_t, wl_fixed_t scale,
                     wl_fixed_t) { static_cast<Log*>(d)->scale = wl_fixed_to_double(scale); },
        .end = [](void* d, zwp_pointer_gesture_pinch_v1*, uint32_t, uint32_t,
                  int32_t) { ++static_cast<Log*>(d)->ends; },
    };
    zwp_pointer_gesture_pinch_v1_add_listener(pinch, &pl, &log);
    auto [s, ss] = in.surface();
    in.seat.pointer_enter(ss, 1, 1);
    pg.pinch_begin(1, 2);
    in.seat.pointer_clear_focus();  // the pointer moves off mid-gesture
    pg.pinch_update(2, 0, 0, 1.5, 0);
    pg.pinch_end(3, false);
    in.pump();
    EXPECT_EQ(log.begins, 1);
    EXPECT_DOUBLE_EQ(log.scale, 1.5);
    EXPECT_EQ(log.ends, 1);
    zwp_pointer_gesture_pinch_v1_destroy(pinch);
    zwp_pointer_gestures_v1_release(m);
    wl_surface_destroy(s);
}

TEST(WlInput, ShortcutInhibitorsAndCursorShapes) {
    Input in;
    wl::ShortcutInhibitors si(in.server);
    wl::CursorShapes cs(in.server, in.seat);
    auto* m = in.bind<zwp_keyboard_shortcuts_inhibit_manager_v1>(&zwp_keyboard_shortcuts_inhibit_manager_v1_interface, 1);
    auto [s, ss] = in.surface();
    auto* inh = zwp_keyboard_shortcuts_inhibit_manager_v1_inhibit_shortcuts(m, s, in.wseat);
    int active = 0;
    static const zwp_keyboard_shortcuts_inhibitor_v1_listener il = {
        .active = [](void* d, zwp_keyboard_shortcuts_inhibitor_v1*) { ++*static_cast<int*>(d); },
        .inactive = [](void* d, zwp_keyboard_shortcuts_inhibitor_v1*) { --*static_cast<int*>(d); },
    };
    zwp_keyboard_shortcuts_inhibitor_v1_add_listener(inh, &il, &active);
    in.pump();
    ASSERT_NE(si.for_surface(ss), nullptr);
    si.set_active(si.for_surface(ss), true);
    in.pump();
    EXPECT_EQ(active, 1);

    std::vector<wl::CursorShapes::Request> asked;
    auto c = cs.request_shape.connect([&](const auto& r) { asked.push_back(r); });
    auto* cm = in.bind<wp_cursor_shape_manager_v1>(&wp_cursor_shape_manager_v1_interface, 2);
    wp_cursor_shape_device_v1* dev = wp_cursor_shape_manager_v1_get_pointer(cm, in.pointer);
    wp_cursor_shape_device_v1_set_shape(dev, 7, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_TEXT);
    in.pump();
    ASSERT_EQ(asked.size(), 1u);
    EXPECT_STREQ(wl::CursorShapes::name_of(asked[0].shape), "text");
    EXPECT_EQ(asked[0].serial, 7u);
    wp_cursor_shape_device_v1_destroy(dev);
    wp_cursor_shape_manager_v1_destroy(cm);
    zwp_keyboard_shortcuts_inhibitor_v1_destroy(inh);
    zwp_keyboard_shortcuts_inhibit_manager_v1_destroy(m);
    wl_surface_destroy(s);
    in.pump();
    EXPECT_EQ(in.error(), 0);
}

TEST(WlInput, IdleNotificationsAndInhibitors) {
    Input in;
    wl::IdleNotifier idle(in.server, in.seat);
    wl::IdleInhibitors inhibitors(in.server);
    auto* m = in.bind<ext_idle_notifier_v1>(&ext_idle_notifier_v1_interface, 2);
    ext_idle_notification_v1* n = ext_idle_notifier_v1_get_idle_notification(m, 30, in.wseat);
    ext_idle_notification_v1* inp = ext_idle_notifier_v1_get_input_idle_notification(m, 30, in.wseat);
    int idled[2] = {0, 0}, resumed[2] = {0, 0};
    struct Pair {
        int* idled;
        int* resumed;
    };
    static const ext_idle_notification_v1_listener nl = {
        .idled = [](void* d, ext_idle_notification_v1*) { ++*static_cast<Pair*>(d)->idled; },
        .resumed = [](void* d, ext_idle_notification_v1*) { ++*static_cast<Pair*>(d)->resumed; },
    };
    Pair p0{&idled[0], &resumed[0]}, p1{&idled[1], &resumed[1]};
    ext_idle_notification_v1_add_listener(n, &nl, &p0);
    ext_idle_notification_v1_add_listener(inp, &nl, &p1);
    in.pump();
    idle.set_inhibited(true);  // a video: only the input-idle one counts down
    usleep(50 * 1000);
    in.pump();
    EXPECT_EQ(idled[0], 0);
    EXPECT_EQ(idled[1], 1);
    idle.activity();
    in.pump();
    EXPECT_EQ(resumed[1], 1);
    idle.set_inhibited(false);
    usleep(50 * 1000);
    in.pump();
    EXPECT_EQ(idled[0], 1);

    auto* im = in.bind<zwp_idle_inhibit_manager_v1>(&zwp_idle_inhibit_manager_v1_interface, 1);
    auto [s, ss] = in.surface();
    zwp_idle_inhibitor_v1* inh = zwp_idle_inhibit_manager_v1_create_inhibitor(im, s);
    in.pump();
    EXPECT_EQ(inhibitors.surfaces(), (std::vector<wl::Surface*>{ss}));
    zwp_idle_inhibitor_v1_destroy(inh);
    in.pump();
    EXPECT_TRUE(inhibitors.surfaces().empty());

    zwp_idle_inhibit_manager_v1_destroy(im);
    ext_idle_notification_v1_destroy(n);
    ext_idle_notification_v1_destroy(inp);
    ext_idle_notifier_v1_destroy(m);
    wl_surface_destroy(s);
    in.pump();
    EXPECT_EQ(in.error(), 0);
}
