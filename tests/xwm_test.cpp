// The XWM (src/xwayland) against a real Xwayland on atrium's own protocol
// layer, with an X11 client made here.
#ifdef ATRIUM_XWAYLAND
#include "wl/compositor.hpp"
#include "wl/data_device.hpp"
#include "wl/output.hpp"
#include "wl/seat.hpp"
#include "wl/selection.hpp"
#include "wl/shm.hpp"
#include "wl/xwayland_shell.hpp"
#include "xwayland/server.hpp"
#include "xwayland/xwm.hpp"

#include <gtest/gtest.h>

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <functional>

using namespace atrium;
using namespace atrium::xwayland;

namespace {

xcb_atom_t intern(xcb_connection_t* c, const char* name) {
    xcb_intern_atom_reply_t* r = xcb_intern_atom_reply(c, xcb_intern_atom(c, 0, uint16_t(strlen(name)), name), nullptr);
    const xcb_atom_t a = r ? r->atom : xcb_atom_t(XCB_ATOM_NONE);
    free(r);
    return a;
}

// A whole Xwayland: atrium's globals, the X server, the XWM, and an X11
// client to drive it.
// Goes last: the globals below are made on it.
struct DisplayOwner {
    wl_display* display = wl_display_create();
    ~DisplayOwner() {
        wl_display_destroy_clients(display);
        wl_display_destroy(display);
    }
};

struct XHarness : DisplayOwner {
    wl::Compositor compositor{display, nullptr};
    wl::Shm shm{display, {}};
    wl::Seat seat{display, "seat0"};
    wl::DataDevices data{display, seat};
    wl::PrimarySelection primary{display, seat};
    wl::Output output{display, wl::OutputInfo{.name = "X-1", .mode_width = 1280, .mode_height = 720,
                                              .refresh = 60000, .logical_width = 1280, .logical_height = 720}};
    std::unique_ptr<wl::XwaylandShell> shell;
    std::unique_ptr<xwayland::Server> server;
    std::unique_ptr<Xwm> xwm;
    std::vector<XSurface*> made;
    wl::Connection made_c;
    wl::Connection start, ready;
    xcb_connection_t* x = nullptr;
    // X11 events for the client, as they came.
    std::vector<xcb_generic_event_t*> xevents;
    std::function<void(xcb_generic_event_t*)> on_xevent;

    explicit XHarness(bool with_shell = true) {
        if (with_shell)
            shell = std::make_unique<wl::XwaylandShell>(display);
        server = std::make_unique<xwayland::Server>(display);
        if (!server->ok()) {
            server.reset();
            return;
        }
        start = server->events.start.connect([this] {
            if (shell)
                shell->set_client(server->client());
        });
        ready = server->events.ready.connect([this](int wm_fd) {
            xwm = std::make_unique<Xwm>(display, wm_fd, server->client(), compositor, shell.get(), false);
            xwm->set_seat(&seat, &data, &primary);
            made_c = xwm->events.new_surface.connect([this](XSurface* s) { made.push_back(s); });
        });
        run_until([this] { return xwm != nullptr; });
        if (xwm)
            x = xcb_connect(server->display_name(), nullptr);
    }

    ~XHarness() {
        for (auto* e : xevents)
            free(e);
        if (x)
            xcb_disconnect(x);
        // Before the server: the XWM's surfaces watch its globals.
        made_c.disconnect();
        xwm.reset();
        start.disconnect();
        ready.disconnect();
        server.reset();
    }

    bool ok() const { return xwm && x && !xcb_connection_has_error(x); }

    // Runs both sides until `done` or a few seconds pass.
    bool run_until(const std::function<bool()>& done, int ms = 5000) {
        wl_event_loop* loop = wl_display_get_event_loop(display);
        const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        while (std::chrono::steady_clock::now() < end) {
            if (done())
                return true;
            wl_display_flush_clients(display);
            wl_event_loop_dispatch(loop, 5);
            if (x) {
                xcb_flush(x);
                while (xcb_generic_event_t* e = xcb_poll_for_event(x)) {
                    if (on_xevent)
                        on_xevent(e);
                    xevents.push_back(e);
                }
            }
        }
        return done();
    }

    xcb_window_t make_window(int w, int h) {
        xcb_screen_t* screen = xcb_setup_roots_iterator(xcb_get_setup(x)).data;
        const xcb_window_t win = xcb_generate_id(x);
        const uint32_t values[] = {screen->white_pixel,
                                   XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_STRUCTURE_NOTIFY |
                                       XCB_EVENT_MASK_PROPERTY_CHANGE};
        xcb_create_window(x, XCB_COPY_FROM_PARENT, win, screen->root, 10, 20, uint16_t(w), uint16_t(h), 0,
                          XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual, XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK,
                          values);
        return win;
    }

    void set_string(xcb_window_t w, xcb_atom_t prop, xcb_atom_t type, const std::string& v) {
        xcb_change_property(x, XCB_PROP_MODE_REPLACE, w, prop, type, 8, uint32_t(v.size()), v.data());
    }

    XSurface* surface_of(xcb_window_t w) {
        for (XSurface* s : made)
            if (s->window_id == w)
                return s;
        return nullptr;
    }

    // A window up and drawn: its XSurface has a mapped wl_surface.
    XSurface* show(xcb_window_t w) {
        xcb_map_window(x, w);
        XSurface* s = nullptr;
        run_until([&] {
            s = surface_of(w);
            return s && s->surface && s->surface->mapped();
        });
        return s;
    }
};

#define REQUIRE_XWAYLAND(h)                                     \
    if (!(h).ok())                                              \
        GTEST_SKIP() << "Xwayland didn't start here";

} // namespace

TEST(Xwm, AWindowMapsWithItsNameAndClass) {
    XHarness h;
    REQUIRE_XWAYLAND(h);
    const xcb_window_t w = h.make_window(200, 100);
    h.set_string(w, intern(h.x, "_NET_WM_NAME"), intern(h.x, "UTF8_STRING"), "hello there");
    h.set_string(w, XCB_ATOM_WM_CLASS, XCB_ATOM_STRING, std::string("probe\0Probe\0", 12));
    XSurface* s = h.show(w);
    ASSERT_NE(s, nullptr);
    ASSERT_NE(s->surface, nullptr);
    EXPECT_TRUE(s->surface->mapped());
    EXPECT_EQ(XSurface::from(s->surface), s);
    EXPECT_EQ(s->title.value_or(""), "hello there");
    EXPECT_EQ(s->class_.value_or(""), "Probe");
    EXPECT_EQ(s->instance.value_or(""), "probe");
    EXPECT_FALSE(s->override_redirect);
    EXPECT_EQ(s->width, 200);

    // Unmapped: it lets go of the surface.
    bool dissociated = false;
    wl::Connection d = s->events.dissociate.connect([&] { dissociated = true; });
    xcb_unmap_window(h.x, w);
    EXPECT_TRUE(h.run_until([&] { return dissociated; }));
    EXPECT_EQ(s->surface, nullptr);
    EXPECT_TRUE(s->withdrawn);
}

// Without xwayland_shell_v1, Xwayland names the surface by its id.
TEST(Xwm, PairsByIdWithoutTheShell) {
    XHarness h(false);
    REQUIRE_XWAYLAND(h);
    XSurface* s = h.show(h.make_window(30, 30));
    ASSERT_NE(s, nullptr);
    EXPECT_NE(s->surface, nullptr);
    EXPECT_EQ(s->serial, 0u);
}

TEST(Xwm, PairsBySerialWithTheShell) {
    XHarness h;
    REQUIRE_XWAYLAND(h);
    XSurface* s = h.show(h.make_window(30, 30));
    ASSERT_NE(s, nullptr);
    EXPECT_NE(s->surface, nullptr);
    EXPECT_NE(s->serial, 0u);
}

TEST(Xwm, TitleChangesAreSeen) {
    XHarness h;
    REQUIRE_XWAYLAND(h);
    const xcb_window_t w = h.make_window(50, 50);
    XSurface* s = h.show(w);
    ASSERT_NE(s, nullptr);
    int titles = 0;
    wl::Connection c = s->events.set_title.connect([&] { ++titles; });
    h.set_string(w, XCB_ATOM_WM_NAME, XCB_ATOM_STRING, "old style");
    ASSERT_TRUE(h.run_until([&] { return titles == 1; }));
    EXPECT_EQ(s->title.value_or(""), "old style");
    // _NET_WM_NAME wins over WM_NAME.
    h.set_string(w, intern(h.x, "_NET_WM_NAME"), intern(h.x, "UTF8_STRING"), "new style");
    ASSERT_TRUE(h.run_until([&] { return titles == 2; }));
    EXPECT_EQ(s->title.value_or(""), "new style");
}

TEST(Xwm, ConfigureRequestsAreAskedAndConfigureMovesIt) {
    XHarness h;
    REQUIRE_XWAYLAND(h);
    const xcb_window_t w = h.make_window(100, 100);
    XSurface* s = h.show(w);
    ASSERT_NE(s, nullptr);
    std::optional<XSurface::ConfigureRequest> asked;
    wl::Connection c = s->events.request_configure.connect([&](const XSurface::ConfigureRequest& r) { asked = r; });
    const uint32_t size[] = {300, 150};
    xcb_configure_window(h.x, w, XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, size);
    ASSERT_TRUE(h.run_until([&] { return asked.has_value(); }));
    EXPECT_EQ(asked->width, 300);
    EXPECT_EQ(asked->height, 150);

    s->configure(40, 60, 300, 150);
    bool seen = false;
    h.on_xevent = [&](xcb_generic_event_t* e) {
        if ((e->response_type & 0x7f) == XCB_CONFIGURE_NOTIFY) {
            auto* n = reinterpret_cast<xcb_configure_notify_event_t*>(e);
            seen = seen || (n->window == w && n->width == 300 && n->x == 40);
        }
    };
    EXPECT_TRUE(h.run_until([&] { return seen; }));
}

TEST(Xwm, CloseAsksWindowsThatSayTheyCanBeAsked) {
    XHarness h;
    REQUIRE_XWAYLAND(h);
    const xcb_window_t w = h.make_window(50, 50);
    const xcb_atom_t protocols = intern(h.x, "WM_PROTOCOLS"), del = intern(h.x, "WM_DELETE_WINDOW");
    xcb_change_property(h.x, XCB_PROP_MODE_REPLACE, w, protocols, XCB_ATOM_ATOM, 32, 1, &del);
    XSurface* s = h.show(w);
    ASSERT_NE(s, nullptr);
    bool asked = false;
    h.on_xevent = [&](xcb_generic_event_t* e) {
        if ((e->response_type & 0x7f) != XCB_CLIENT_MESSAGE)
            return;
        auto* m = reinterpret_cast<xcb_client_message_event_t*>(e);
        asked = asked || (m->type == protocols && m->data.data32[0] == del);
    };
    s->close();
    EXPECT_TRUE(h.run_until([&] { return asked; }));
}

TEST(Xwm, StateRequestsComeOutAsSignals) {
    XHarness h;
    REQUIRE_XWAYLAND(h);
    const xcb_window_t w = h.make_window(50, 50);
    XSurface* s = h.show(w);
    ASSERT_NE(s, nullptr);
    int fullscreen = 0, above = 0;
    std::optional<bool> minimize;
    wl::Connection a = s->events.request_fullscreen.connect([&] { ++fullscreen; });
    wl::Connection b = s->events.request_above.connect([&] { ++above; });
    wl::Connection m = s->events.request_minimize.connect([&](bool on) { minimize = on; });

    xcb_screen_t* screen = xcb_setup_roots_iterator(xcb_get_setup(h.x)).data;
    auto send = [&](xcb_atom_t type, uint32_t d0, uint32_t d1, uint32_t d2) {
        xcb_client_message_event_t ev{};
        ev.response_type = XCB_CLIENT_MESSAGE;
        ev.format = 32;
        ev.window = w;
        ev.type = type;
        ev.data.data32[0] = d0;
        ev.data.data32[1] = d1;
        ev.data.data32[2] = d2;
        xcb_send_event(h.x, 0, screen->root,
                       XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY | XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT,
                       reinterpret_cast<const char*>(&ev));
    };
    const xcb_atom_t state = intern(h.x, "_NET_WM_STATE");
    send(state, 1, intern(h.x, "_NET_WM_STATE_FULLSCREEN"), intern(h.x, "_NET_WM_STATE_ABOVE"));
    ASSERT_TRUE(h.run_until([&] { return fullscreen == 1 && above == 1; }));
    EXPECT_TRUE(s->fullscreen);
    EXPECT_TRUE(s->above);
    // Toggled off again.
    send(state, 2, intern(h.x, "_NET_WM_STATE_FULLSCREEN"), 0);
    ASSERT_TRUE(h.run_until([&] { return fullscreen == 2; }));
    EXPECT_FALSE(s->fullscreen);
    // WM_CHANGE_STATE iconic: minimize.
    send(intern(h.x, "WM_CHANGE_STATE"), XCB_ICCCM_WM_STATE_ICONIC, 0, 0);
    ASSERT_TRUE(h.run_until([&] { return minimize.has_value(); }));
    EXPECT_TRUE(*minimize);
}

TEST(Xwm, TransientForMakesAParent) {
    XHarness h;
    REQUIRE_XWAYLAND(h);
    const xcb_window_t main = h.make_window(200, 200), dialog = h.make_window(50, 50);
    xcb_change_property(h.x, XCB_PROP_MODE_REPLACE, dialog, XCB_ATOM_WM_TRANSIENT_FOR, XCB_ATOM_WINDOW, 32, 1, &main);
    XSurface* m = h.show(main);
    XSurface* d = h.show(dialog);
    ASSERT_NE(m, nullptr);
    ASSERT_NE(d, nullptr);
    EXPECT_EQ(d->parent, m);
    ASSERT_EQ(m->children.size(), 1u);
    EXPECT_EQ(m->children[0], d);
    // The parent goes: the dialog is told.
    bool told = false;
    wl::Connection c = d->events.set_parent.connect([&] { told = true; });
    xcb_destroy_window(h.x, main);
    ASSERT_TRUE(h.run_until([&] { return told; }));
    EXPECT_EQ(d->parent, nullptr);
}

TEST(Xwm, ActivatingFocusesAndSetsTheActiveWindow) {
    XHarness h;
    REQUIRE_XWAYLAND(h);
    const xcb_window_t w = h.make_window(50, 50);
    XSurface* s = h.show(w);
    ASSERT_NE(s, nullptr);
    s->activate(true);
    EXPECT_EQ(h.xwm->focused(), s);
    xcb_screen_t* screen = xcb_setup_roots_iterator(xcb_get_setup(h.x)).data;
    const xcb_atom_t active = intern(h.x, "_NET_ACTIVE_WINDOW");
    xcb_window_t got = 0;
    EXPECT_TRUE(h.run_until([&] {
        xcb_get_property_reply_t* r =
            xcb_get_property_reply(h.x, xcb_get_property(h.x, 0, screen->root, active, XCB_ATOM_WINDOW, 0, 1), nullptr);
        if (r && xcb_get_property_value_length(r) == 4)
            got = *static_cast<xcb_window_t*>(xcb_get_property_value(r));
        free(r);
        return got == w;
    }));
    xcb_get_input_focus_reply_t* f = xcb_get_input_focus_reply(h.x, xcb_get_input_focus(h.x), nullptr);
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(f->focus, w);
    free(f);
}

// An X11 window's copy becomes the Wayland clipboard, and reads back.
TEST(Xwm, X11ClipboardReachesWayland) {
    XHarness h;
    REQUIRE_XWAYLAND(h);
    const xcb_window_t w = h.make_window(50, 50);
    XSurface* s = h.show(w);
    ASSERT_NE(s, nullptr);
    s->activate(true);  // only a focused window may set it
    const xcb_atom_t clipboard = intern(h.x, "CLIPBOARD"), targets = intern(h.x, "TARGETS"),
                     utf8 = intern(h.x, "UTF8_STRING");
    h.on_xevent = [&](xcb_generic_event_t* e) {
        if ((e->response_type & 0x7f) != XCB_SELECTION_REQUEST)
            return;
        auto* r = reinterpret_cast<xcb_selection_request_event_t*>(e);
        bool ok = true;
        if (r->target == targets) {
            const xcb_atom_t t[] = {targets, utf8};
            xcb_change_property(h.x, XCB_PROP_MODE_REPLACE, r->requestor, r->property, XCB_ATOM_ATOM, 32, 2, t);
        } else if (r->target == utf8) {
            h.set_string(r->requestor, r->property, utf8, "from x11");
        } else {
            ok = false;
        }
        xcb_selection_notify_event_t n{};
        n.response_type = XCB_SELECTION_NOTIFY;
        n.time = r->time;
        n.requestor = r->requestor;
        n.selection = r->selection;
        n.target = r->target;
        n.property = ok ? r->property : xcb_atom_t(XCB_ATOM_NONE);
        xcb_send_event(h.x, 0, r->requestor, XCB_EVENT_MASK_NO_EVENT, reinterpret_cast<const char*>(&n));
    };
    xcb_set_selection_owner(h.x, w, clipboard, XCB_CURRENT_TIME);
    ASSERT_TRUE(h.run_until([&] { return h.data.selection() != nullptr; }));
    wl::DataSource* src = h.data.selection();
    EXPECT_TRUE(src->offers("text/plain;charset=utf-8"));

    int p[2];
    ASSERT_EQ(pipe2(p, O_CLOEXEC | O_NONBLOCK), 0);
    src->send("text/plain;charset=utf-8", p[1]);
    std::string got;
    bool eof = false;
    h.run_until([&] {
        char buf[256];
        ssize_t n;
        while ((n = read(p[0], buf, sizeof(buf))) > 0)
            got.append(buf, size_t(n));
        eof = n == 0;
        return eof;
    });
    close(p[0]);
    EXPECT_TRUE(eof);
    EXPECT_EQ(got, "from x11");
}

// Only the focused X11 window may set the clipboard.
TEST(Xwm, AnUnfocusedWindowCantSetTheClipboard) {
    XHarness h;
    REQUIRE_XWAYLAND(h);
    const xcb_window_t w = h.make_window(50, 50);
    ASSERT_NE(h.show(w), nullptr);
    const xcb_atom_t clipboard = intern(h.x, "CLIPBOARD"), targets = intern(h.x, "TARGETS");
    bool asked = false;
    h.on_xevent = [&](xcb_generic_event_t* e) {
        if ((e->response_type & 0x7f) != XCB_SELECTION_REQUEST)
            return;
        auto* r = reinterpret_cast<xcb_selection_request_event_t*>(e);
        asked = true;
        const xcb_atom_t t[] = {targets};
        xcb_change_property(h.x, XCB_PROP_MODE_REPLACE, r->requestor, r->property, XCB_ATOM_ATOM, 32, 1, t);
        xcb_selection_notify_event_t n{};
        n.response_type = XCB_SELECTION_NOTIFY;
        n.requestor = r->requestor;
        n.selection = r->selection;
        n.target = r->target;
        n.property = r->property;
        xcb_send_event(h.x, 0, r->requestor, XCB_EVENT_MASK_NO_EVENT, reinterpret_cast<const char*>(&n));
    };
    xcb_set_selection_owner(h.x, w, clipboard, XCB_CURRENT_TIME);
    ASSERT_TRUE(h.run_until([&] { return asked; }));
    h.run_until([] { return false; }, 200);
    EXPECT_EQ(h.data.selection(), nullptr);
}

namespace {
// atrium's own text, as a Wayland client's would be.
struct TextSource : wl::DataSource {
    std::string text;
    explicit TextSource(std::string t) : text(std::move(t)) { mime_types_ = {"text/plain;charset=utf-8"}; }
    void send(const std::string&, int fd) override {
        (void)!write(fd, text.data(), text.size());
        close(fd);
    }
};
} // namespace

// A Wayland clipboard reaches an X11 window that asks for it.
TEST(Xwm, WaylandClipboardReachesX11) {
    XHarness h;
    REQUIRE_XWAYLAND(h);
    const xcb_window_t w = h.make_window(50, 50);
    XSurface* s = h.show(w);
    ASSERT_NE(s, nullptr);
    s->activate(true);
    TextSource text("from wayland");
    h.data.set_selection(&text);
    const xcb_atom_t clipboard = intern(h.x, "CLIPBOARD"), utf8 = intern(h.x, "UTF8_STRING"),
                     prop = intern(h.x, "PROBE_DATA");
    // atrium's window owns the X11 selection now.
    ASSERT_TRUE(h.run_until([&] {
        xcb_get_selection_owner_reply_t* r =
            xcb_get_selection_owner_reply(h.x, xcb_get_selection_owner(h.x, clipboard), nullptr);
        const bool owned = r && r->owner != XCB_WINDOW_NONE;
        free(r);
        return owned;
    }));
    bool notified = false;
    h.on_xevent = [&](xcb_generic_event_t* e) {
        if ((e->response_type & 0x7f) == XCB_SELECTION_NOTIFY)
            notified = reinterpret_cast<xcb_selection_notify_event_t*>(e)->property == prop;
    };
    xcb_convert_selection(h.x, w, clipboard, utf8, prop, XCB_CURRENT_TIME);
    ASSERT_TRUE(h.run_until([&] { return notified; }));
    xcb_get_property_reply_t* r =
        xcb_get_property_reply(h.x, xcb_get_property(h.x, 1, w, prop, XCB_GET_PROPERTY_TYPE_ANY, 0, 1024), nullptr);
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(std::string(static_cast<const char*>(xcb_get_property_value(r)), size_t(xcb_get_property_value_length(r))),
              "from wayland");
    free(r);
    h.data.set_selection(nullptr);
}

TEST(Xwm, OverrideRedirectWindowsAreUnmanaged) {
    XHarness h;
    REQUIRE_XWAYLAND(h);
    xcb_screen_t* screen = xcb_setup_roots_iterator(xcb_get_setup(h.x)).data;
    const xcb_window_t w = xcb_generate_id(h.x);
    const uint32_t values[] = {screen->white_pixel, 1};
    xcb_create_window(h.x, XCB_COPY_FROM_PARENT, w, screen->root, 5, 5, 40, 40, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
                      screen->root_visual, XCB_CW_BACK_PIXEL | XCB_CW_OVERRIDE_REDIRECT, values);
    XSurface* s = h.show(w);
    ASSERT_NE(s, nullptr);
    EXPECT_TRUE(s->override_redirect);
    // Not given focus.
    s->activate(true);
    EXPECT_EQ(h.xwm->focused(), nullptr);
}
#endif
