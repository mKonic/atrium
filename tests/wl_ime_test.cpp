// Text input, input methods and virtual input (src/wl/ime) against a real
// libwayland client.
#include "wl/compositor.hpp"
#include "wl/ime.hpp"
#include "wl/seat.hpp"
#include "wl/shm.hpp"
#include "wl_harness.hpp"

#include "input-method-unstable-v2-client-protocol.h"
#include "text-input-unstable-v3-client-protocol.h"
#include "virtual-keyboard-unstable-v1-client-protocol.h"
#include "wlr-virtual-pointer-unstable-v1-client-protocol.h"

#include <wayland-client-protocol.h>

#include <gtest/gtest.h>

#include <sys/mman.h>
#include <unistd.h>

using namespace atrium;

namespace {

struct Ime : wltest::Harness {
    wl::Compositor compositor{server, nullptr};
    wl::Shm shm{server, {}};
    wl::Seat seat{server, "seat0"};
    wl::TextInputs texts{server, seat};
    wl::InputMethods methods{server, seat};
    wl::VirtualInputs virtuals{server, seat};
    std::vector<wl::Surface*> surfaces;
    wl::Connection made = compositor.new_surface.connect([this](wl::Surface* s) { surfaces.push_back(s); });
    wl_compositor* comp = nullptr;
    wl_seat* wseat = nullptr;
    wl_shm* wshm = nullptr;
    Ime() {
        seat.set_capabilities(wl::Seat::Keyboard);
        comp = bind<wl_compositor>(&wl_compositor_interface);
        wshm = bind<struct wl_shm>(&wl_shm_interface, 2);
        wseat = bind<wl_seat>(&wl_seat_interface);
        pump();
    }
    ~Ime() {
        if (!client)
            return;
        wl_seat_release(wseat);
        wl_shm_release(wshm);
        wl_compositor_destroy(comp);
        pump();
    }
    wl_buffer* buffer(int w, int h) {
        int fd = memfd_create("ime-test", MFD_CLOEXEC);
        EXPECT_EQ(ftruncate(fd, w * h * 4), 0);
        wl_shm_pool* pool = wl_shm_create_pool(wshm, fd, w * h * 4);
        wl_buffer* b = wl_shm_pool_create_buffer(pool, 0, w, h, w * 4, WL_SHM_FORMAT_ARGB8888);
        wl_shm_pool_destroy(pool);
        close(fd);
        return b;
    }
};

} // namespace

TEST(WlIme, TextInputLifecycleAndReplies) {
    Ime t;
    auto* m = t.bind<zwp_text_input_manager_v3>(&zwp_text_input_manager_v3_interface, 1);
    zwp_text_input_v3* ti = zwp_text_input_manager_v3_get_text_input(m, t.wseat);
    struct Log {
        int enters = 0;
        std::string preedit, committed;
        uint32_t done_serial = 0;
    } log;
    static const zwp_text_input_v3_listener tl = {
        .enter = [](void* d, zwp_text_input_v3*, wl_surface*) { ++static_cast<Log*>(d)->enters; },
        .leave = [](void*, zwp_text_input_v3*, wl_surface*) {},
        .preedit_string = [](void* d, zwp_text_input_v3*, const char* s, int32_t,
                             int32_t) { static_cast<Log*>(d)->preedit = s ? s : ""; },
        .commit_string = [](void* d, zwp_text_input_v3*,
                            const char* s) { static_cast<Log*>(d)->committed = s ? s : ""; },
        .delete_surrounding_text = [](void*, zwp_text_input_v3*, uint32_t, uint32_t) {},
        .done = [](void* d, zwp_text_input_v3*, uint32_t serial) { static_cast<Log*>(d)->done_serial = serial; },
    };
    zwp_text_input_v3_add_listener(ti, &tl, &log);
    wl_surface* s = wl_compositor_create_surface(t.comp);
    t.pump();
    wl::Surface* ss = t.surfaces.back();
    t.texts.focus(ss);
    t.pump();
    EXPECT_EQ(log.enters, 1);

    std::vector<std::string> seen;
    auto c1 = t.texts.events.enable.connect([&](auto*) { seen.push_back("enable"); });
    auto c2 = t.texts.events.commit.connect([&](auto*) { seen.push_back("commit"); });
    auto c3 = t.texts.events.disable.connect([&](auto*) { seen.push_back("disable"); });
    zwp_text_input_v3_enable(ti);
    zwp_text_input_v3_set_surrounding_text(ti, "hello", 5, 5);
    zwp_text_input_v3_commit(ti);
    zwp_text_input_v3_set_cursor_rectangle(ti, 10, 20, 1, 16);
    zwp_text_input_v3_commit(ti);
    zwp_text_input_v3_disable(ti);
    zwp_text_input_v3_commit(ti);
    t.pump();
    EXPECT_EQ(seen, (std::vector<std::string>{"enable", "commit", "disable"}));

    wl::TextInputs::TextInput* input = t.texts.inputs()[0].get();
    EXPECT_EQ(input->commits, 3u);
    t.texts.send_preedit(input, "ni", 2, 2);
    t.texts.send_commit(input, "你");
    t.texts.send_done(input);
    t.pump();
    EXPECT_EQ(log.preedit, "ni");
    EXPECT_EQ(log.committed, "你");
    EXPECT_EQ(log.done_serial, 3u);  // the number of commits it answers

    zwp_text_input_v3_destroy(ti);
    zwp_text_input_manager_v3_destroy(m);
    wl_surface_destroy(s);
    t.pump();
    EXPECT_EQ(t.error(), 0);
}

TEST(WlIme, OneInputMethodPerSeatAndStaleCommitsDrop) {
    Ime t;
    auto* m = t.bind<zwp_input_method_manager_v2>(&zwp_input_method_manager_v2_interface, 1);
    zwp_input_method_v2* im = zwp_input_method_manager_v2_get_input_method(m, t.wseat);
    zwp_input_method_v2* second = zwp_input_method_manager_v2_get_input_method(m, t.wseat);
    struct Log {
        int activated = 0, dones = 0, unavailable = 0;
    } log1, log2;
    static const zwp_input_method_v2_listener il = {
        .activate = [](void* d, zwp_input_method_v2*) { ++static_cast<Log*>(d)->activated; },
        .deactivate = [](void*, zwp_input_method_v2*) {},
        .surrounding_text = [](void*, zwp_input_method_v2*, const char*, uint32_t, uint32_t) {},
        .text_change_cause = [](void*, zwp_input_method_v2*, uint32_t) {},
        .content_type = [](void*, zwp_input_method_v2*, uint32_t, uint32_t) {},
        .done = [](void* d, zwp_input_method_v2*) { ++static_cast<Log*>(d)->dones; },
        .unavailable = [](void* d, zwp_input_method_v2*) { ++static_cast<Log*>(d)->unavailable; },
    };
    zwp_input_method_v2_add_listener(im, &il, &log1);
    zwp_input_method_v2_add_listener(second, &il, &log2);
    t.pump();
    EXPECT_EQ(log2.unavailable, 1);
    ASSERT_NE(t.methods.current(), nullptr);

    std::vector<std::string> committed;
    auto c = t.methods.events.commit.connect([&](wl::InputMethods::InputMethod* x) {
        committed.push_back(x->current.commit.value_or(""));
    });
    t.methods.activate({});
    t.pump();
    EXPECT_EQ(log1.activated, 1);
    EXPECT_EQ(log1.dones, 1);
    zwp_input_method_v2_commit_string(im, "stale");
    zwp_input_method_v2_commit(im, 0);  // answers nothing we sent (1 done so far)
    zwp_input_method_v2_commit_string(im, "fresh");
    zwp_input_method_v2_commit(im, 1);
    t.pump();
    EXPECT_EQ(committed, (std::vector<std::string>{"fresh"}));

    // The keyboard grab gets the keymap and keys.
    t.methods.grab_keymap("xkb_keymap {};");
    zwp_input_method_keyboard_grab_v2* g = zwp_input_method_v2_grab_keyboard(im);
    struct Grab {
        std::string keymap;
        std::vector<uint32_t> keys;
    } grab;
    static const zwp_input_method_keyboard_grab_v2_listener gl = {
        .keymap =
            [](void* d, zwp_input_method_keyboard_grab_v2*, uint32_t, int32_t fd, uint32_t size) {
                void* p = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
                static_cast<Grab*>(d)->keymap.assign(static_cast<char*>(p), size - 1);
                munmap(p, size);
                close(fd);
            },
        .key = [](void* d, zwp_input_method_keyboard_grab_v2*, uint32_t, uint32_t, uint32_t key,
                  uint32_t) { static_cast<Grab*>(d)->keys.push_back(key); },
        .modifiers = [](void*, zwp_input_method_keyboard_grab_v2*, uint32_t, uint32_t, uint32_t, uint32_t,
                        uint32_t) {},
        .repeat_info = [](void*, zwp_input_method_keyboard_grab_v2*, int32_t, int32_t) {},
    };
    zwp_input_method_keyboard_grab_v2_add_listener(g, &gl, &grab);
    t.pump();
    EXPECT_TRUE(t.methods.grabbed());
    t.methods.grab_key(1, 30, true);
    t.pump();
    EXPECT_EQ(grab.keymap, "xkb_keymap {};");
    EXPECT_EQ(grab.keys, (std::vector<uint32_t>{30}));
    zwp_input_method_keyboard_grab_v2_release(g);
    t.pump();
    EXPECT_FALSE(t.methods.grabbed());

    zwp_input_method_v2_destroy(second);
    zwp_input_method_v2_destroy(im);
    zwp_input_method_manager_v2_destroy(m);
    t.pump();
    EXPECT_EQ(t.methods.current(), nullptr);
    EXPECT_EQ(t.error(), 0);
}

TEST(WlIme, VirtualKeyboardNeedsAKeymapFirst) {
    Ime t;
    wl::VirtualInputs::Keyboard* made = nullptr;
    std::vector<uint32_t> keys;
    wl::Connection keyc;
    auto c = t.virtuals.new_keyboard.connect([&](wl::VirtualInputs::Keyboard* k) {
        made = k;
        keyc = k->key.connect([&](uint32_t, uint32_t key, bool) { keys.push_back(key); });
    });
    auto* m = t.bind<zwp_virtual_keyboard_manager_v1>(&zwp_virtual_keyboard_manager_v1_interface, 1);
    zwp_virtual_keyboard_v1* vk = zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(m, t.wseat);
    const std::string keymap = "xkb_keymap { virtual };";
    int fd = memfd_create("vk", MFD_CLOEXEC);
    ASSERT_EQ(write(fd, keymap.c_str(), keymap.size() + 1), ssize_t(keymap.size() + 1));
    zwp_virtual_keyboard_v1_keymap(vk, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fd, uint32_t(keymap.size() + 1));
    close(fd);
    zwp_virtual_keyboard_v1_key(vk, 1, 44, WL_KEYBOARD_KEY_STATE_PRESSED);
    t.pump();
    ASSERT_NE(made, nullptr);
    EXPECT_EQ(made->keymap, keymap);
    EXPECT_EQ(keys, (std::vector<uint32_t>{44}));

    zwp_virtual_keyboard_v1* bare = zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(m, t.wseat);
    zwp_virtual_keyboard_v1_key(bare, 1, 44, WL_KEYBOARD_KEY_STATE_PRESSED);
    t.pump();
    EXPECT_TRUE(t.posted("zwp_virtual_keyboard_v1", ZWP_VIRTUAL_KEYBOARD_V1_ERROR_NO_KEYMAP));
    zwp_virtual_keyboard_v1_destroy(bare);
    zwp_virtual_keyboard_v1_destroy(vk);
    wl_proxy_destroy(reinterpret_cast<wl_proxy*>(m));  // the manager has no destroy request
}

TEST(WlIme, VirtualPointerMoves) {
    Ime t;
    std::vector<std::pair<double, double>> moves;
    wl::Connection mc;
    auto c = t.virtuals.new_pointer.connect([&](wl::VirtualInputs::Pointer* p) {
        mc = p->motion_absolute.connect([&](uint32_t, double x, double y) { moves.push_back({x, y}); });
    });
    auto* m = t.bind<zwlr_virtual_pointer_manager_v1>(&zwlr_virtual_pointer_manager_v1_interface, 2);
    zwlr_virtual_pointer_v1* vp = zwlr_virtual_pointer_manager_v1_create_virtual_pointer(m, t.wseat);
    zwlr_virtual_pointer_v1_motion_absolute(vp, 1, 50, 25, 100, 100);
    zwlr_virtual_pointer_v1_frame(vp);
    t.pump();
    ASSERT_EQ(moves.size(), 1u);
    EXPECT_DOUBLE_EQ(moves[0].first, 0.5);
    EXPECT_DOUBLE_EQ(moves[0].second, 0.25);
    zwlr_virtual_pointer_v1_destroy(vp);
    zwlr_virtual_pointer_manager_v1_destroy(m);
    t.pump();
    EXPECT_EQ(t.error(), 0);
}

TEST(WlIme, PopupShowsWhileItHasContent) {
    Ime t;
    auto* m = t.bind<zwp_input_method_manager_v2>(&zwp_input_method_manager_v2_interface, 1);
    zwp_input_method_v2* im = zwp_input_method_manager_v2_get_input_method(m, t.wseat);
    wl_surface* s = wl_compositor_create_surface(t.comp);
    zwp_input_popup_surface_v2* popup = zwp_input_method_v2_get_input_popup_surface(im, s);
    t.pump();
    wl::Surface* ss = t.surfaces.back();
    EXPECT_FALSE(ss->mapped());

    wl_buffer* b = t.buffer(30, 10);
    wl_surface_attach(s, b, 0, 0);
    wl_surface_commit(s);
    t.pump();
    EXPECT_TRUE(ss->mapped());  // fcitx5's candidates: never shown before
    wl_surface_attach(s, nullptr, 0, 0);
    wl_surface_commit(s);
    t.pump();
    EXPECT_FALSE(ss->mapped());

    // Destroying the popup hides it.
    wl_surface_attach(s, b, 0, 0);
    wl_surface_commit(s);
    zwp_input_popup_surface_v2_destroy(popup);
    t.pump();
    EXPECT_FALSE(ss->mapped());
    EXPECT_EQ(t.error(), 0);

    wl_buffer_destroy(b);
    wl_surface_destroy(s);
    zwp_input_method_v2_destroy(im);
    zwp_input_method_manager_v2_destroy(m);
    t.pump();
    EXPECT_EQ(t.error(), 0);
}
