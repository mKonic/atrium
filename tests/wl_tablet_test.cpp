// zwp_tablet_manager_v2 (src/wl/tablet) against a real libwayland client.
#include "wl/compositor.hpp"
#include "wl/seat.hpp"
#include "wl/tablet.hpp"
#include "wl_harness.hpp"

#include "tablet-v2-client-protocol.h"

#include <wayland-client-protocol.h>

#include <gtest/gtest.h>

using namespace atrium;

namespace {

// What the client heard, and the objects it has to destroy.
struct Log {
    std::vector<zwp_tablet_v2*> tablets;
    std::vector<zwp_tablet_tool_v2*> tools;
    std::vector<zwp_tablet_pad_v2*> pads;
    std::vector<zwp_tablet_pad_group_v2*> groups;
    std::vector<zwp_tablet_pad_ring_v2*> rings;
    std::string tablet_name;
    uint32_t tool_type = 0, pad_buttons = 0, modes = 0;
    int tablet_done = 0, tool_done = 0, pad_done = 0, removed = 0;
    uint32_t proximity_serial = 0, down_serial = 0, pressure = 0;
    int proximity_in = 0, proximity_out = 0, downs = 0, ups = 0, frames = 0;
    double x = 0, y = 0;
    int pad_enters = 0, pad_leaves = 0, pad_buttons_pressed = 0;
    double angle = -1;
    int ring_frames = 0;
};

void listen(Log* log, zwp_tablet_v2* t) {
    static const zwp_tablet_v2_listener l = {
        .name = [](void* d, zwp_tablet_v2*, const char* n) { static_cast<Log*>(d)->tablet_name = n; },
        .id = [](void*, zwp_tablet_v2*, uint32_t, uint32_t) {},
        .path = [](void*, zwp_tablet_v2*, const char*) {},
        .done = [](void* d, zwp_tablet_v2*) { ++static_cast<Log*>(d)->tablet_done; },
        .removed = [](void* d, zwp_tablet_v2*) { ++static_cast<Log*>(d)->removed; },
        .bustype = [](void*, zwp_tablet_v2*, uint32_t) {},
    };
    zwp_tablet_v2_add_listener(t, &l, log);
}

void listen(Log* log, zwp_tablet_tool_v2* t) {
    static const zwp_tablet_tool_v2_listener l = {
        .type = [](void* d, zwp_tablet_tool_v2*, uint32_t type) { static_cast<Log*>(d)->tool_type = type; },
        .hardware_serial = [](void*, zwp_tablet_tool_v2*, uint32_t, uint32_t) {},
        .hardware_id_wacom = [](void*, zwp_tablet_tool_v2*, uint32_t, uint32_t) {},
        .capability = [](void*, zwp_tablet_tool_v2*, uint32_t) {},
        .done = [](void* d, zwp_tablet_tool_v2*) { ++static_cast<Log*>(d)->tool_done; },
        .removed = [](void* d, zwp_tablet_tool_v2*) { ++static_cast<Log*>(d)->removed; },
        .proximity_in =
            [](void* d, zwp_tablet_tool_v2*, uint32_t serial, zwp_tablet_v2*, wl_surface*) {
                auto* l = static_cast<Log*>(d);
                ++l->proximity_in;
                l->proximity_serial = serial;
            },
        .proximity_out = [](void* d, zwp_tablet_tool_v2*) { ++static_cast<Log*>(d)->proximity_out; },
        .down =
            [](void* d, zwp_tablet_tool_v2*, uint32_t serial) {
                ++static_cast<Log*>(d)->downs;
                static_cast<Log*>(d)->down_serial = serial;
            },
        .up = [](void* d, zwp_tablet_tool_v2*) { ++static_cast<Log*>(d)->ups; },
        .motion =
            [](void* d, zwp_tablet_tool_v2*, wl_fixed_t x, wl_fixed_t y) {
                static_cast<Log*>(d)->x = wl_fixed_to_double(x);
                static_cast<Log*>(d)->y = wl_fixed_to_double(y);
            },
        .pressure = [](void* d, zwp_tablet_tool_v2*, uint32_t p) { static_cast<Log*>(d)->pressure = p; },
        .distance = [](void*, zwp_tablet_tool_v2*, uint32_t) {},
        .tilt = [](void*, zwp_tablet_tool_v2*, wl_fixed_t, wl_fixed_t) {},
        .rotation = [](void*, zwp_tablet_tool_v2*, wl_fixed_t) {},
        .slider = [](void*, zwp_tablet_tool_v2*, int32_t) {},
        .wheel = [](void*, zwp_tablet_tool_v2*, wl_fixed_t, int32_t) {},
        .button = [](void*, zwp_tablet_tool_v2*, uint32_t, uint32_t, uint32_t) {},
        .frame = [](void* d, zwp_tablet_tool_v2*, uint32_t) { ++static_cast<Log*>(d)->frames; },
    };
    zwp_tablet_tool_v2_add_listener(t, &l, log);
}

void listen(Log* log, zwp_tablet_pad_ring_v2* r) {
    static const zwp_tablet_pad_ring_v2_listener l = {
        .source = [](void*, zwp_tablet_pad_ring_v2*, uint32_t) {},
        .angle = [](void* d, zwp_tablet_pad_ring_v2*, wl_fixed_t a) { static_cast<Log*>(d)->angle = wl_fixed_to_double(a); },
        .stop = [](void*, zwp_tablet_pad_ring_v2*) {},
        .frame = [](void* d, zwp_tablet_pad_ring_v2*, uint32_t) { ++static_cast<Log*>(d)->ring_frames; },
    };
    zwp_tablet_pad_ring_v2_add_listener(r, &l, log);
}

void listen(Log* log, zwp_tablet_pad_group_v2* g) {
    static const zwp_tablet_pad_group_v2_listener l = {
        .buttons = [](void*, zwp_tablet_pad_group_v2*, wl_array*) {},
        .ring =
            [](void* d, zwp_tablet_pad_group_v2*, zwp_tablet_pad_ring_v2* r) {
                static_cast<Log*>(d)->rings.push_back(r);
                listen(static_cast<Log*>(d), r);
            },
        .strip = [](void*, zwp_tablet_pad_group_v2*, zwp_tablet_pad_strip_v2*) {},
        .modes = [](void* d, zwp_tablet_pad_group_v2*, uint32_t m) { static_cast<Log*>(d)->modes = m; },
        .done = [](void*, zwp_tablet_pad_group_v2*) {},
        .mode_switch = [](void*, zwp_tablet_pad_group_v2*, uint32_t, uint32_t, uint32_t) {},
        .dial = [](void*, zwp_tablet_pad_group_v2*, zwp_tablet_pad_dial_v2*) {},
    };
    zwp_tablet_pad_group_v2_add_listener(g, &l, log);
}

void listen(Log* log, zwp_tablet_pad_v2* p) {
    static const zwp_tablet_pad_v2_listener l = {
        .group =
            [](void* d, zwp_tablet_pad_v2*, zwp_tablet_pad_group_v2* g) {
                static_cast<Log*>(d)->groups.push_back(g);
                listen(static_cast<Log*>(d), g);
            },
        .path = [](void*, zwp_tablet_pad_v2*, const char*) {},
        .buttons = [](void* d, zwp_tablet_pad_v2*, uint32_t n) { static_cast<Log*>(d)->pad_buttons = n; },
        .done = [](void* d, zwp_tablet_pad_v2*) { ++static_cast<Log*>(d)->pad_done; },
        .button =
            [](void* d, zwp_tablet_pad_v2*, uint32_t, uint32_t, uint32_t state) {
                static_cast<Log*>(d)->pad_buttons_pressed += state;
            },
        .enter = [](void* d, zwp_tablet_pad_v2*, uint32_t, zwp_tablet_v2*,
                    wl_surface*) { ++static_cast<Log*>(d)->pad_enters; },
        .leave = [](void* d, zwp_tablet_pad_v2*, uint32_t, wl_surface*) { ++static_cast<Log*>(d)->pad_leaves; },
        .removed = [](void* d, zwp_tablet_pad_v2*) { ++static_cast<Log*>(d)->removed; },
    };
    zwp_tablet_pad_v2_add_listener(p, &l, log);
}

struct Tab : wltest::Harness {
    wl::Compositor compositor{server, nullptr};
    wl::Seat seat{server, "seat0"};
    wl::Tablets tablets{server, seat};
    std::vector<wl::Surface*> surfaces;
    wl::Connection made = compositor.new_surface.connect([this](wl::Surface* s) { surfaces.push_back(s); });
    wl_compositor* comp = nullptr;
    wl_seat* wseat = nullptr;
    zwp_tablet_manager_v2* manager = nullptr;
    zwp_tablet_seat_v2* tseat = nullptr;
    Log log;

    Tab() {
        comp = bind<wl_compositor>(&wl_compositor_interface);
        wseat = bind<wl_seat>(&wl_seat_interface);
        manager = bind<zwp_tablet_manager_v2>(&zwp_tablet_manager_v2_interface, 2);
        pump();
    }
    ~Tab() {
        if (!client)
            return;
        for (auto* r : log.rings)
            zwp_tablet_pad_ring_v2_destroy(r);
        for (auto* g : log.groups)
            zwp_tablet_pad_group_v2_destroy(g);
        for (auto* p : log.pads)
            zwp_tablet_pad_v2_destroy(p);
        for (auto* t : log.tools)
            zwp_tablet_tool_v2_destroy(t);
        for (auto* t : log.tablets)
            zwp_tablet_v2_destroy(t);
        if (tseat)
            zwp_tablet_seat_v2_destroy(tseat);
        zwp_tablet_manager_v2_destroy(manager);
        wl_seat_release(wseat);
        wl_compositor_destroy(comp);
        pump();
    }
    // A tablet seat for `on`, its announcements logged.
    zwp_tablet_seat_v2* tablet_seat(wl_seat* on) {
        zwp_tablet_seat_v2* ts = zwp_tablet_manager_v2_get_tablet_seat(manager, on);
        static const zwp_tablet_seat_v2_listener l = {
            .tablet_added =
                [](void* d, zwp_tablet_seat_v2*, zwp_tablet_v2* t) {
                    static_cast<Log*>(d)->tablets.push_back(t);
                    listen(static_cast<Log*>(d), t);
                },
            .tool_added =
                [](void* d, zwp_tablet_seat_v2*, zwp_tablet_tool_v2* t) {
                    static_cast<Log*>(d)->tools.push_back(t);
                    listen(static_cast<Log*>(d), t);
                },
            .pad_added =
                [](void* d, zwp_tablet_seat_v2*, zwp_tablet_pad_v2* p) {
                    static_cast<Log*>(d)->pads.push_back(p);
                    listen(static_cast<Log*>(d), p);
                },
        };
        zwp_tablet_seat_v2_add_listener(ts, &l, &log);
        pump();
        return ts;
    }
    void get_seat() { tseat = tablet_seat(wseat); }
    std::pair<wl_surface*, wl::Surface*> surface() {
        wl_surface* s = wl_compositor_create_surface(comp);
        pump();
        return {s, surfaces.back()};
    }
};

} // namespace

TEST(WlTablet, DevicesAreAnnouncedBeforeAndAfterBinding) {
    Tab t;
    t.tablets.add_tablet({.name = "Wacom Intuos"});
    t.get_seat();  // an existing tablet is announced on binding...
    ASSERT_EQ(t.log.tablets.size(), 1u);
    EXPECT_EQ(t.log.tablet_name, "Wacom Intuos");
    EXPECT_EQ(t.log.tablet_done, 1);

    // ...and a new tool or pad as it comes.
    t.tablets.add_tool({.type = ZWP_TABLET_TOOL_V2_TYPE_ERASER});
    t.tablets.add_pad({.buttons = 4, .groups = {{.buttons = {0, 1, 2, 3}, .rings = 1, .modes = 2}}});
    t.pump();
    EXPECT_EQ(t.log.tool_type, uint32_t(ZWP_TABLET_TOOL_V2_TYPE_ERASER));
    EXPECT_EQ(t.log.tool_done, 1);
    EXPECT_EQ(t.log.pad_buttons, 4u);
    EXPECT_EQ(t.log.groups.size(), 1u);
    EXPECT_EQ(t.log.rings.size(), 1u);
    EXPECT_EQ(t.log.modes, 2u);
    EXPECT_EQ(t.log.pad_done, 1);
    EXPECT_EQ(t.error(), 0);
}

TEST(WlTablet, ToolEventsGoToTheSurfaceUnderIt) {
    Tab t;
    auto* tablet = t.tablets.add_tablet({.name = "pen tablet"});
    auto* tool = t.tablets.add_tool({});
    t.get_seat();
    auto [s, ss] = t.surface();

    t.tablets.proximity_in(tool, tablet, ss, 10, 20);
    t.tablets.frame(tool, 1);
    t.tablets.down(tool);
    t.tablets.pressure(tool, 0.5);
    t.tablets.motion(tool, 11, 21);
    t.tablets.frame(tool, 2);
    t.pump();
    EXPECT_EQ(t.log.proximity_in, 1);
    EXPECT_EQ(t.log.downs, 1);
    EXPECT_EQ(t.log.pressure, 32768u);
    EXPECT_EQ(t.log.x, 11);
    EXPECT_EQ(t.log.frames, 2);
    // A tool's down is a grab serial, like a button press.
    EXPECT_TRUE(t.seat.validate_grab_serial(t.peer, t.log.down_serial));

    // Leaving lifts the tool first, and ends with a frame.
    t.tablets.proximity_out(tool);
    t.pump();
    EXPECT_EQ(t.log.ups, 1);
    EXPECT_EQ(t.log.proximity_out, 1);
    EXPECT_EQ(t.log.frames, 3);
    EXPECT_EQ(t.tablets.focus(tool), nullptr);
    wl_surface_destroy(s);
    t.pump();
    EXPECT_EQ(t.error(), 0);
}

TEST(WlTablet, CursorNeedsTheProximitySerial) {
    Tab t;
    auto* tablet = t.tablets.add_tablet({});
    auto* tool = t.tablets.add_tool({});
    t.get_seat();
    auto [s, ss] = t.surface();
    auto [cursor, cs] = t.surface();
    std::vector<wl::Surface*> asked;
    wl::Connection c =
        t.tablets.events.request_cursor.connect([&](const wl::Tablets::CursorRequest& r) { asked.push_back(r.surface); });

    t.tablets.proximity_in(tool, tablet, ss, 0, 0);
    t.tablets.frame(tool, 1);
    t.pump();
    zwp_tablet_tool_v2_set_cursor(t.log.tools[0], t.log.proximity_serial + 1, cursor, 0, 0);  // stale
    t.pump();
    EXPECT_TRUE(asked.empty());
    zwp_tablet_tool_v2_set_cursor(t.log.tools[0], t.log.proximity_serial, cursor, 2, 3);
    t.pump();
    ASSERT_EQ(asked.size(), 1u);
    EXPECT_EQ(asked[0], cs);
    EXPECT_STREQ(cs->role_name(), "zwp_tablet_tool_v2-cursor");

    wl_surface_destroy(cursor);
    wl_surface_destroy(s);
    t.pump();
    EXPECT_EQ(t.error(), 0);
}

TEST(WlTablet, PadsEnterAndSendControls) {
    Tab t;
    auto* tablet = t.tablets.add_tablet({});
    auto* pad = t.tablets.add_pad({.buttons = 2, .groups = {{.buttons = {0, 1}, .rings = 1, .modes = 1}}});
    t.get_seat();
    auto [s, ss] = t.surface();

    t.tablets.pad_enter(pad, tablet, ss);
    t.tablets.pad_button(pad, 1, 0, true);
    t.tablets.pad_ring(pad, 0, 90.0, true, 2);
    t.pump();
    EXPECT_EQ(t.log.pad_enters, 1);
    EXPECT_EQ(t.log.pad_buttons_pressed, 1);
    EXPECT_EQ(t.log.angle, 90);
    EXPECT_EQ(t.log.ring_frames, 1);

    // The surface going drops the focus without a leave (nothing to leave).
    wl_surface_destroy(s);
    t.pump();
    EXPECT_EQ(t.tablets.focus(pad), nullptr);
    t.tablets.pad_button(pad, 3, 1, true);
    t.pump();
    EXPECT_EQ(t.log.pad_buttons_pressed, 1);
    EXPECT_EQ(t.error(), 0);
}

TEST(WlTablet, RemovalTellsTheClient) {
    Tab t;
    auto* tablet = t.tablets.add_tablet({});
    auto* tool = t.tablets.add_tool({});
    t.get_seat();
    auto [s, ss] = t.surface();
    t.tablets.proximity_in(tool, tablet, ss, 0, 0);
    t.tablets.frame(tool, 1);
    // The tablet goes with the tool over it: the tool leaves, then it's removed.
    t.tablets.remove(tablet);
    t.tablets.remove(tool);
    t.pump();
    EXPECT_EQ(t.log.proximity_out, 1);
    EXPECT_EQ(t.log.removed, 2);
    wl_surface_destroy(s);
    t.pump();
    EXPECT_EQ(t.error(), 0);
}

TEST(WlTablet, AnotherSeatHearsNothing) {
    Tab t;
    wl::Seat other{t.server, "seat1"};
    t.tablets.add_tablet({});
    t.pump();
    // Bind the second seat: the last wl_seat advertised.
    wl_seat* second = nullptr;
    for (auto it = t.globals.rbegin(); it != t.globals.rend(); ++it)
        if (it->interface == "wl_seat") {
            second = static_cast<wl_seat*>(wl_registry_bind(t.registry, it->name, &wl_seat_interface, 5));
            break;
        }
    ASSERT_NE(second, nullptr);
    auto* ts = t.tablet_seat(second);
    EXPECT_TRUE(t.log.tablets.empty());
    zwp_tablet_seat_v2_destroy(ts);
    wl_seat_release(second);
    t.pump();
    EXPECT_EQ(t.error(), 0);
}
