// The seat, clipboard, drag and drop, primary selection and data control
// (src/wl) against a real libwayland client.
#include "wl/compositor.hpp"
#include "wl/data_device.hpp"
#include "wl/seat.hpp"
#include "wl/selection.hpp"
#include "wl/shm.hpp"
#include "wl_harness.hpp"

#include "ext-data-control-v1-client-protocol.h"
#include "primary-selection-unstable-v1-client-protocol.h"

#include <wayland-client-protocol.h>

#include <gtest/gtest.h>

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <string>

using namespace atrium;

namespace {

struct World : wltest::Harness {
    wl::Compositor compositor{server, nullptr};
    wl::Shm shm{server, {}};
    wl::Seat seat{server, "seat0"};
    wl::DataDevices data{server, seat};
    wl::PrimarySelection primary{server, seat};
    wl::DataControl control{server, seat, data.slot(), primary.slot()};
    std::vector<wl::Surface*> surfaces;
    wl::Signal<wl::Surface*>::Connection made =
        compositor.new_surface.connect([this](wl::Surface* s) { surfaces.push_back(s); });

    wl_compositor* comp = nullptr;
    wl_seat* wseat = nullptr;

    World() {
        seat.set_capabilities(wl::Seat::Pointer | wl::Seat::Keyboard);
        comp = bind<wl_compositor>(&wl_compositor_interface);
        wseat = bind<wl_seat>(&wl_seat_interface);
        pump();
    }
    ~World() {
        if (client) {
            wl_compositor_destroy(comp);
            wl_seat_release(wseat);
        }
    }

    std::pair<wl_surface*, wl::Surface*> surface() {
        wl_surface* s = wl_compositor_create_surface(comp);
        pump();
        return {s, surfaces.back()};
    }
};

// Reads what a pipe gets until it closes.
std::string read_all(int fd) {
    std::string out;
    char buf[256];
    ssize_t n;
    while ((n = read(fd, buf, sizeof buf)) > 0)
        out.append(buf, size_t(n));
    close(fd);
    return out;
}

struct KeyboardLog {
    std::string keymap;
    int entered = 0, left = 0;
    std::vector<uint32_t> enter_keys, keys;
    uint32_t mods_depressed = 0;
    int rate = 0;
};

const wl_keyboard_listener kKeyboard = {
    .keymap =
        [](void* d, wl_keyboard*, uint32_t, int32_t fd, uint32_t size) {
            auto* log = static_cast<KeyboardLog*>(d);
            if (size) {
                void* p = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
                if (p != MAP_FAILED) {
                    log->keymap.assign(static_cast<char*>(p), size - 1);
                    munmap(p, size);
                }
            }
            close(fd);
        },
    .enter =
        [](void* d, wl_keyboard*, uint32_t, wl_surface*, wl_array* keys) {
            auto* log = static_cast<KeyboardLog*>(d);
            ++log->entered;
            auto* k = static_cast<uint32_t*>(keys->data);
            log->enter_keys.assign(k, k + keys->size / 4);
        },
    .leave = [](void* d, wl_keyboard*, uint32_t, wl_surface*) { ++static_cast<KeyboardLog*>(d)->left; },
    .key = [](void* d, wl_keyboard*, uint32_t, uint32_t, uint32_t key,
              uint32_t state) { static_cast<KeyboardLog*>(d)->keys.push_back(state ? key : 1000 + key); },
    .modifiers = [](void* d, wl_keyboard*, uint32_t, uint32_t dep, uint32_t, uint32_t,
                    uint32_t) { static_cast<KeyboardLog*>(d)->mods_depressed = dep; },
    .repeat_info = [](void* d, wl_keyboard*, int32_t rate, int32_t) { static_cast<KeyboardLog*>(d)->rate = rate; },
};

} // namespace

TEST(WlSeat, KeyboardFollowsFocus) {
    World w;
    w.seat.set_keymap("xkb_keymap { fake };");
    w.seat.set_repeat_info(33, 200);
    KeyboardLog log;
    wl_keyboard* kb = wl_seat_get_keyboard(w.wseat);
    wl_keyboard_add_listener(kb, &kKeyboard, &log);
    auto [s1, ss1] = w.surface();
    auto [s2, ss2] = w.surface();
    w.pump();
    EXPECT_EQ(log.keymap, "xkb_keymap { fake };");
    EXPECT_EQ(log.rate, 33);

    w.seat.keyboard_enter(ss1, {30, 31}, {.depressed = 4});
    w.seat.keyboard_key(10, 32, true);
    w.seat.keyboard_key(11, 32, false);
    w.pump();
    EXPECT_EQ(log.entered, 1);
    EXPECT_EQ(log.enter_keys, (std::vector<uint32_t>{30, 31}));
    EXPECT_EQ(log.mods_depressed, 4u);
    EXPECT_EQ(log.keys, (std::vector<uint32_t>{32, 1032}));

    w.seat.keyboard_enter(ss2, {}, {});
    w.pump();
    EXPECT_EQ(log.left, 1);
    EXPECT_EQ(log.entered, 2);

    // A focused surface that goes takes the focus with it.
    wl_surface_destroy(s2);
    w.pump();
    EXPECT_EQ(w.seat.keyboard_focus(), nullptr);
    w.seat.keyboard_key(12, 40, true);  // nobody to send it to
    w.pump();
    EXPECT_EQ(log.keys.size(), 2u);

    wl_keyboard_release(kb);
    wl_surface_destroy(s1);
    w.pump();
    EXPECT_EQ(w.error(), 0);
}

TEST(WlSeat, PointerEventsAndCursorRequests) {
    World w;
    struct Log {
        int enters = 0, frames = 0, value120 = 0;
        uint32_t enter_serial = 0, last_button = 0;
        double x = 0;
    } log;
    static const wl_pointer_listener pl = {
        .enter =
            [](void* d, wl_pointer*, uint32_t serial, wl_surface*, wl_fixed_t x, wl_fixed_t) {
                auto* l = static_cast<Log*>(d);
                ++l->enters;
                l->enter_serial = serial;
                l->x = wl_fixed_to_double(x);
            },
        .leave = [](void*, wl_pointer*, uint32_t, wl_surface*) {},
        .motion = [](void* d, wl_pointer*, uint32_t, wl_fixed_t x,
                     wl_fixed_t) { static_cast<Log*>(d)->x = wl_fixed_to_double(x); },
        .button = [](void* d, wl_pointer*, uint32_t, uint32_t, uint32_t b,
                     uint32_t) { static_cast<Log*>(d)->last_button = b; },
        .axis = [](void*, wl_pointer*, uint32_t, uint32_t, wl_fixed_t) {},
        .frame = [](void* d, wl_pointer*) { ++static_cast<Log*>(d)->frames; },
        .axis_source = [](void*, wl_pointer*, uint32_t) {},
        .axis_stop = [](void*, wl_pointer*, uint32_t, uint32_t) {},
        .axis_discrete = [](void*, wl_pointer*, uint32_t, int32_t) {},
        .axis_value120 = [](void* d, wl_pointer*, uint32_t, int32_t v) { static_cast<Log*>(d)->value120 = v; },
        .axis_relative_direction = [](void*, wl_pointer*, uint32_t, uint32_t) {},
    };
    wl_pointer* ptr = wl_seat_get_pointer(w.wseat);
    wl_pointer_add_listener(ptr, &pl, &log);
    auto [s, ss] = w.surface();
    auto [cursor, scursor] = w.surface();
    w.seat.pointer_enter(ss, 5.5, 6);
    w.seat.pointer_motion(1, 7.25, 8);
    w.seat.pointer_frame();
    w.seat.pointer_axis(2, 0, 15, 120, wl::Seat::AxisSource::Wheel, false);
    w.seat.pointer_frame();
    const uint32_t press = w.seat.pointer_button(3, 272, true);
    w.seat.pointer_frame();
    w.pump();
    EXPECT_EQ(log.enters, 1);
    EXPECT_DOUBLE_EQ(log.x, 7.25);
    EXPECT_EQ(log.value120, 120);
    EXPECT_EQ(log.last_button, 272u);
    EXPECT_EQ(log.frames, 4);  // after the enter, and one per batch
    EXPECT_TRUE(w.seat.validate_grab_serial(w.peer, press));
    EXPECT_FALSE(w.seat.validate_grab_serial(w.peer, press + 1000));

    std::vector<wl::Seat::CursorRequest> asked;
    auto conn = w.seat.events.request_cursor.connect([&](const auto& r) { asked.push_back(r); });
    wl_pointer_set_cursor(ptr, log.enter_serial - 1, cursor, 1, 2);  // stale serial: ignored
    wl_pointer_set_cursor(ptr, log.enter_serial, cursor, 3, 4);
    w.pump();
    ASSERT_EQ(asked.size(), 1u);
    EXPECT_EQ(asked[0].surface, scursor);
    EXPECT_EQ(asked[0].hotspot_x, 3);
    EXPECT_STREQ(scursor->role_name(), "wl_pointer-cursor");

    wl_pointer_release(ptr);
    wl_surface_destroy(s);
    wl_surface_destroy(cursor);
    w.pump();
    EXPECT_EQ(w.error(), 0);
}

TEST(WlSeat, ClipboardReachesTheFocusedClient) {
    World w;
    wl_data_device_manager* mgr = w.bind<wl_data_device_manager>(&wl_data_device_manager_interface, 3);
    wl_data_device* dev = wl_data_device_manager_get_data_device(mgr, w.wseat);
    struct Log {
        wl_data_offer* offer = nullptr;
        std::vector<std::string> types;
        wl_data_source* sent_by = nullptr;
    } log;
    static const wl_data_offer_listener ol = {
        .offer = [](void* d, wl_data_offer*, const char* m) { static_cast<Log*>(d)->types.emplace_back(m); },
        .source_actions = [](void*, wl_data_offer*, uint32_t) {},
        .action = [](void*, wl_data_offer*, uint32_t) {},
    };
    static const wl_data_device_listener dl = {
        .data_offer = [](void* d, wl_data_device*,
                         wl_data_offer* o) { wl_data_offer_add_listener(o, &ol, d); },
        .enter = [](void*, wl_data_device*, uint32_t, wl_surface*, wl_fixed_t, wl_fixed_t, wl_data_offer*) {},
        .leave = [](void*, wl_data_device*) {},
        .motion = [](void*, wl_data_device*, uint32_t, wl_fixed_t, wl_fixed_t) {},
        .drop = [](void*, wl_data_device*) {},
        .selection =
            [](void* d, wl_data_device*, wl_data_offer* o) {
                auto* l = static_cast<Log*>(d);
                if (l->offer)
                    wl_data_offer_destroy(l->offer);  // a client drops the old one
                l->offer = o;
            },
    };
    wl_data_device_add_listener(dev, &dl, &log);

    wl_data_source* src = wl_data_device_manager_create_data_source(mgr);
    static const wl_data_source_listener sl = {
        .target = [](void*, wl_data_source*, const char*) {},
        .send =
            [](void*, wl_data_source*, const char* mime, int32_t fd) {
                std::string text = std::string("hello as ") + mime;
                (void)!write(fd, text.data(), text.size());
                close(fd);
            },
        .cancelled = [](void*, wl_data_source*) {},
        .dnd_drop_performed = [](void*, wl_data_source*) {},
        .dnd_finished = [](void*, wl_data_source*) {},
        .action = [](void*, wl_data_source*, uint32_t) {},
    };
    wl_data_source_add_listener(src, &sl, nullptr);
    wl_data_source_offer(src, "text/plain");
    wl_data_source_offer(src, "text/plain");  // once is enough
    wl_data_source_offer(src, "text/html");
    wl_data_device_set_selection(dev, src, 0);
    w.pump();
    ASSERT_NE(w.data.selection(), nullptr);
    EXPECT_EQ(log.offer, nullptr);  // not focused: nothing offered yet

    auto [s, ss] = w.surface();
    w.seat.keyboard_enter(ss, {}, {});
    w.pump();
    ASSERT_NE(log.offer, nullptr);
    EXPECT_EQ(log.types, (std::vector<std::string>{"text/plain", "text/html"}));

    int p[2];
    ASSERT_EQ(pipe2(p, O_CLOEXEC), 0);
    wl_data_offer_receive(log.offer, "text/html", p[1]);
    close(p[1]);
    w.pump();
    w.pump();
    EXPECT_EQ(read_all(p[0]), "hello as text/html");

    // The source going empties the selection.
    wl_data_source_destroy(src);
    w.pump();
    EXPECT_EQ(w.data.selection(), nullptr);
    EXPECT_EQ(log.offer, nullptr);  // told: no selection

    wl_data_device_release(dev);
    wl_data_device_manager_destroy(mgr);
    wl_surface_destroy(s);
    w.pump();
    EXPECT_EQ(w.error(), 0);
}

TEST(WlSeat, DragAndDropNegotiatesAndFinishes) {
    World w;
    wl_data_device_manager* mgr = w.bind<wl_data_device_manager>(&wl_data_device_manager_interface, 3);
    wl_data_device* dev = wl_data_device_manager_get_data_device(mgr, w.wseat);
    struct Log {
        wl_data_offer* offer = nullptr;
        int drops = 0, performed = 0, finished = 0, cancelled = 0;
        uint32_t source_action = 0;
        std::string target;
    } log;
    static const wl_data_device_listener dl = {
        .data_offer = [](void*, wl_data_device*, wl_data_offer*) {},
        .enter = [](void* d, wl_data_device*, uint32_t, wl_surface*, wl_fixed_t, wl_fixed_t,
                    wl_data_offer* o) { static_cast<Log*>(d)->offer = o; },
        .leave = [](void*, wl_data_device*) {},
        .motion = [](void*, wl_data_device*, uint32_t, wl_fixed_t, wl_fixed_t) {},
        .drop = [](void* d, wl_data_device*) { ++static_cast<Log*>(d)->drops; },
        .selection = [](void*, wl_data_device*, wl_data_offer*) {},
    };
    wl_data_device_add_listener(dev, &dl, &log);
    static const wl_data_source_listener sl = {
        .target = [](void* d, wl_data_source*, const char* m) { static_cast<Log*>(d)->target = m ? m : ""; },
        .send = [](void*, wl_data_source*, const char*, int32_t fd) { close(fd); },
        .cancelled = [](void* d, wl_data_source*) { ++static_cast<Log*>(d)->cancelled; },
        .dnd_drop_performed = [](void* d, wl_data_source*) { ++static_cast<Log*>(d)->performed; },
        .dnd_finished = [](void* d, wl_data_source*) { ++static_cast<Log*>(d)->finished; },
        .action = [](void* d, wl_data_source*, uint32_t a) { static_cast<Log*>(d)->source_action = a; },
    };
    wl_data_source* src = wl_data_device_manager_create_data_source(mgr);
    wl_data_source_add_listener(src, &sl, &log);
    wl_data_source_offer(src, "text/uri-list");
    wl_data_source_set_actions(src, WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY | WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE);

    auto [origin, sorigin] = w.surface();
    auto [target, starget] = w.surface();
    w.seat.pointer_enter(sorigin, 1, 1);
    const uint32_t press = w.seat.pointer_button(1, 272, true);
    // A drag needs the press serial; a made-up one is ignored.
    wl_data_device_start_drag(dev, src, origin, nullptr, press + 77);
    w.pump();
    EXPECT_EQ(w.data.drag(), nullptr);
    wl_data_device_start_drag(dev, src, origin, nullptr, press);
    w.pump();
    wl::Drag* drag = w.data.drag();
    ASSERT_NE(drag, nullptr);

    drag->motion(starget, 10, 10, 5);
    w.pump();
    ASSERT_NE(log.offer, nullptr);
    wl_data_offer_accept(log.offer, 1, "text/uri-list");
    wl_data_offer_set_actions(log.offer, WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE,
                              WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE);
    w.pump();
    EXPECT_EQ(log.target, "text/uri-list");
    EXPECT_EQ(log.source_action, uint32_t(WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE));

    w.data.drag()->drop(6);
    w.pump();
    EXPECT_EQ(w.data.drag(), nullptr);
    EXPECT_EQ(log.drops, 1);
    EXPECT_EQ(log.performed, 1);
    wl_data_offer_finish(log.offer);
    w.pump();
    EXPECT_EQ(log.finished, 1);
    EXPECT_EQ(log.cancelled, 0);

    wl_data_offer_destroy(log.offer);
    wl_data_source_destroy(src);
    wl_data_device_release(dev);
    wl_data_device_manager_destroy(mgr);
    wl_surface_destroy(origin);
    wl_surface_destroy(target);
    w.pump();
    EXPECT_EQ(w.error(), 0);
}

TEST(WlSeat, DropWithoutAgreementCancels) {
    EXPECT_EQ(wl::dnd::choose(wl::dnd::Copy | wl::dnd::Move, wl::dnd::Move, wl::dnd::Copy), wl::dnd::Move);
    EXPECT_EQ(wl::dnd::choose(wl::dnd::Copy | wl::dnd::Move, wl::dnd::Copy | wl::dnd::Move, wl::dnd::Move),
              wl::dnd::Move);
    EXPECT_EQ(wl::dnd::choose(wl::dnd::Copy, wl::dnd::Move, 0), wl::dnd::None);
}

TEST(WlSeat, DataControlSeesAndSetsBothSelections) {
    World w;
    auto* mgr = w.bind<ext_data_control_manager_v1>(&ext_data_control_manager_v1_interface, 1);
    ext_data_control_device_v1* dev = ext_data_control_manager_v1_get_data_device(mgr, w.wseat);
    struct Log {
        ext_data_control_offer_v1 *selection = nullptr, *primary = nullptr;
        int selections = 0;
        std::vector<std::string> types;
    } log;
    static const ext_data_control_offer_v1_listener ol = {
        .offer = [](void* d, ext_data_control_offer_v1*,
                    const char* m) { static_cast<Log*>(d)->types.emplace_back(m); },
    };
    static const ext_data_control_device_v1_listener dl = {
        .data_offer = [](void* d, ext_data_control_device_v1*,
                         ext_data_control_offer_v1* o) { ext_data_control_offer_v1_add_listener(o, &ol, d); },
        .selection =
            [](void* d, ext_data_control_device_v1*, ext_data_control_offer_v1* o) {
                auto* l = static_cast<Log*>(d);
                if (l->selection)
                    ext_data_control_offer_v1_destroy(l->selection);
                l->selection = o;
                ++l->selections;
            },
        .finished = [](void*, ext_data_control_device_v1*) {},
        .primary_selection =
            [](void* d, ext_data_control_device_v1*, ext_data_control_offer_v1* o) {
                auto* l = static_cast<Log*>(d);
                if (l->primary)
                    ext_data_control_offer_v1_destroy(l->primary);
                l->primary = o;
            },
    };
    ext_data_control_device_v1_add_listener(dev, &dl, &log);
    w.pump();
    EXPECT_EQ(log.selections, 1);  // told at once: nothing selected
    EXPECT_EQ(log.selection, nullptr);

    // Without focus, it sets the clipboard, and hears it back.
    ext_data_control_source_v1* src = ext_data_control_manager_v1_create_data_source(mgr);
    static const ext_data_control_source_v1_listener sl = {
        .send =
            [](void*, ext_data_control_source_v1*, const char*, int32_t fd) {
                (void)!write(fd, "from control", 12);
                close(fd);
            },
        .cancelled = [](void*, ext_data_control_source_v1*) {},
    };
    ext_data_control_source_v1_add_listener(src, &sl, nullptr);
    ext_data_control_source_v1_offer(src, "text/plain;charset=utf-8");
    ext_data_control_device_v1_set_selection(dev, src);
    w.pump();
    ASSERT_NE(w.data.selection(), nullptr);
    EXPECT_EQ(w.data.selection()->mime_types(), (std::vector<std::string>{"text/plain;charset=utf-8"}));
    ASSERT_NE(log.selection, nullptr);

    int p[2];
    ASSERT_EQ(pipe2(p, O_CLOEXEC), 0);
    ext_data_control_offer_v1_receive(log.selection, "text/plain;charset=utf-8", p[1]);
    close(p[1]);
    w.pump();
    w.pump();
    EXPECT_EQ(read_all(p[0]), "from control");

    // A source goes in once.
    ext_data_control_device_v1_set_primary_selection(dev, src);
    w.pump();
    EXPECT_EQ(w.protocol_error(), uint32_t(EXT_DATA_CONTROL_DEVICE_V1_ERROR_USED_SOURCE));
    for (auto* o : {log.selection, log.primary})
        if (o)
            ext_data_control_offer_v1_destroy(o);
    ext_data_control_source_v1_destroy(src);
    ext_data_control_device_v1_destroy(dev);
    ext_data_control_manager_v1_destroy(mgr);
}

TEST(WlSeat, PrimarySelectionReachesTheFocusedClient) {
    World w;
    auto* mgr = w.bind<zwp_primary_selection_device_manager_v1>(&zwp_primary_selection_device_manager_v1_interface, 1);
    zwp_primary_selection_device_v1* dev = zwp_primary_selection_device_manager_v1_get_device(mgr, w.wseat);
    zwp_primary_selection_offer_v1* got = nullptr;
    static const zwp_primary_selection_device_v1_listener dl = {
        .data_offer = [](void*, zwp_primary_selection_device_v1*, zwp_primary_selection_offer_v1*) {},
        .selection =
            [](void* d, zwp_primary_selection_device_v1*, zwp_primary_selection_offer_v1* o) {
                auto** got = static_cast<zwp_primary_selection_offer_v1**>(d);
                if (*got)
                    zwp_primary_selection_offer_v1_destroy(*got);
                *got = o;
            },
    };
    zwp_primary_selection_device_v1_add_listener(dev, &dl, &got);
    zwp_primary_selection_source_v1* src = zwp_primary_selection_device_manager_v1_create_source(mgr);
    zwp_primary_selection_source_v1_offer(src, "text/plain");
    zwp_primary_selection_device_v1_set_selection(dev, src, 0);
    auto [s, ss] = w.surface();
    w.seat.keyboard_enter(ss, {}, {});
    w.pump();
    EXPECT_NE(w.primary.slot().get(), nullptr);
    EXPECT_NE(got, nullptr);
    zwp_primary_selection_source_v1_destroy(src);
    w.pump();
    EXPECT_EQ(w.primary.slot().get(), nullptr);
    if (got)
        zwp_primary_selection_offer_v1_destroy(got);
    zwp_primary_selection_device_v1_destroy(dev);
    zwp_primary_selection_device_manager_v1_destroy(mgr);
    wl_surface_destroy(s);
    w.pump();
    EXPECT_EQ(w.error(), 0);
}
