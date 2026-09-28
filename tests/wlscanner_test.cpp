// tools/wlscanner.py's bindings, run for real: a libwayland client and server
// in one process over a socketpair, speaking tests/protocols/atrium-test-v1.xml.
#include "atrium-test-v1-server.hpp"
#include "atrium-test-v1-client-protocol.h"

#include <wayland-client.h>

#include <gtest/gtest.h>

#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace atrium;
using wl::AtriumTestItemV1;
using wl::AtriumTestManagerV1;

namespace {

struct Harness {
    wl_display* server = wl_display_create();
    wl_display* client = nullptr;
    wl_client* peer = nullptr;
    std::unique_ptr<wl::Global> global;
    std::vector<AtriumTestManagerV1*> managers;  // as bound, server side
    std::function<void(AtriumTestManagerV1*)> setup;
    atrium_test_manager_v1* manager = nullptr;  // client side
    uint32_t bind_version = 2;

    explicit Harness(uint32_t version = 2) : bind_version(version) {
        global = wl::Global::create<AtriumTestManagerV1>(server, AtriumTestManagerV1::kVersion,
            [this](wl_client* c, uint32_t v, uint32_t id) {
                if (auto* m = wl::make<AtriumTestManagerV1>(c, v, id)) {
                    managers.push_back(m);
                    if (setup)
                        setup(m);
                }
            });
        int fds[2];
        socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds);
        peer = wl_client_create(server, fds[0]);
        client = wl_display_connect_to_fd(fds[1]);
    }

    ~Harness() {
        if (manager)
            atrium_test_manager_v1_destroy(manager);
        if (client)
            wl_display_disconnect(client);
        global.reset();
        wl_display_destroy_clients(server);
        wl_display_destroy(server);
    }

    // Moves every queued message both ways until both sides go quiet.
    void pump() {
        for (int i = 0; i < 8; ++i) {
            wl_display_flush(client);
            wl_event_loop_dispatch(wl_display_get_event_loop(server), 0);
            wl_display_flush_clients(server);
            while (wl_display_prepare_read(client) != 0)
                if (wl_display_dispatch_pending(client) < 0)
                    return;  // a protocol error: the test's assertions say which
            pollfd p{wl_display_get_fd(client), POLLIN, 0};
            if (poll(&p, 1, 0) > 0)
                wl_display_read_events(client);
            else
                wl_display_cancel_read(client);
            wl_display_dispatch_pending(client);
        }
    }

    // Binds the manager from the client; returns the server's object.
    AtriumTestManagerV1* bind(const void* listener = nullptr, void* data = nullptr) {
        wl_registry* registry = wl_display_get_registry(client);
        static const wl_registry_listener rl = {
            .global =
                [](void* d, wl_registry* r, uint32_t name, const char* iface, uint32_t) {
                    auto* h = static_cast<Harness*>(d);
                    if (std::strcmp(iface, atrium_test_manager_v1_interface.name) == 0)
                        h->manager = static_cast<atrium_test_manager_v1*>(
                            wl_registry_bind(r, name, &atrium_test_manager_v1_interface, h->bind_version));
                },
            .global_remove = [](void*, wl_registry*, uint32_t) {},
        };
        wl_registry_add_listener(registry, &rl, this);
        pump();
        wl_registry_destroy(registry);
        if (manager && listener)
            atrium_test_manager_v1_add_listener(
                manager, static_cast<const atrium_test_manager_v1_listener*>(listener), data);
        pump();
        return managers.empty() ? nullptr : managers.back();
    }
};

struct Events {
    int32_t i = 0;
    double f = 0;
    std::string s;
    std::vector<uint32_t> later;
    atrium_test_item_v1* item_made = nullptr;
};

const atrium_test_manager_v1_listener kListener = {
    .echo =
        [](void* d, atrium_test_manager_v1*, int32_t i, wl_fixed_t f, const char* s) {
            auto* e = static_cast<Events*>(d);
            e->i = i;
            e->f = wl_fixed_to_double(f);
            e->s = s;
        },
    .item_made = [](void* d, atrium_test_manager_v1*,
                    atrium_test_item_v1* item) { static_cast<Events*>(d)->item_made = item; },
    .later = [](void* d, atrium_test_manager_v1*, uint32_t u) { static_cast<Events*>(d)->later.push_back(u); },
};

} // namespace

TEST(WlScanner, RequestArgumentsArriveTyped) {
    Harness h;
    int32_t i = 0;
    uint32_t u = 0;
    double f = 0;
    std::string s = "unset";
    bool null_string = false;
    std::vector<uint8_t> bytes;
    h.setup = [&](AtriumTestManagerV1* m) {
        m->on_scalars([&](AtriumTestManagerV1*, int32_t a, uint32_t b, double c) {
            i = a;
            u = b;
            f = c;
        });
        m->on_text([&](AtriumTestManagerV1*, const char* str, wl_array* arr) {
            if (str)
                s = str;
            else
                null_string = true;
            auto* p = static_cast<uint8_t*>(arr->data);
            bytes.assign(p, p + arr->size);
        });
    };
    ASSERT_NE(h.bind(), nullptr);

    atrium_test_manager_v1_scalars(h.manager, -7, uint32_t(AtriumTestManagerV1::Flags::Second),
                                   wl_fixed_from_double(1.5));
    wl_array arr;
    wl_array_init(&arr);
    uint8_t* p = static_cast<uint8_t*>(wl_array_add(&arr, 3));
    p[0] = 1, p[1] = 2, p[2] = 250;
    atrium_test_manager_v1_text(h.manager, "hello", &arr);
    h.pump();
    EXPECT_EQ(i, -7);
    EXPECT_EQ(u, 2u);
    EXPECT_DOUBLE_EQ(f, 1.5);
    EXPECT_EQ(s, "hello");
    EXPECT_EQ(bytes, (std::vector<uint8_t>{1, 2, 250}));

    atrium_test_manager_v1_text(h.manager, nullptr, &arr);
    h.pump();
    EXPECT_TRUE(null_string);
    wl_array_release(&arr);
}

TEST(WlScanner, FdIsHandedOver) {
    Harness h;
    std::string got;
    h.setup = [&](AtriumTestManagerV1* m) {
        m->on_pass_fd([&](AtriumTestManagerV1*, int fd) {
            char buf[16] = {};
            ssize_t n = read(fd, buf, sizeof buf);
            got.assign(buf, n > 0 ? size_t(n) : 0);
            close(fd);
        });
    };
    ASSERT_NE(h.bind(), nullptr);
    int pipefd[2];
    ASSERT_EQ(pipe(pipefd), 0);
    ASSERT_EQ(write(pipefd[1], "fd!", 3), 3);
    close(pipefd[1]);
    atrium_test_manager_v1_pass_fd(h.manager, pipefd[0]);
    close(pipefd[0]);  // libwayland duplicated it
    h.pump();
    EXPECT_EQ(got, "fd!");
}

TEST(WlScanner, EventsCarryValuesAndRespectTheirVersion) {
    for (uint32_t version : {1u, 2u}) {
        Harness h(version);
        Events e;
        AtriumTestManagerV1* m = h.bind(&kListener, &e);
        ASSERT_NE(m, nullptr);
        EXPECT_EQ(m->version(), version);
        m->send_echo(-3, 0.25, "back");
        m->send_later(9);
        h.pump();
        EXPECT_EQ(e.i, -3);
        EXPECT_DOUBLE_EQ(e.f, 0.25);
        EXPECT_EQ(e.s, "back");
        // `later` is since="2": a version 1 client never sees it.
        EXPECT_EQ(e.later, version >= 2 ? std::vector<uint32_t>{9} : std::vector<uint32_t>{});
    }
}

TEST(WlScanner, NewIdAndTypedObjects) {
    Harness h;
    Events e;
    std::vector<AtriumTestItemV1*> made;
    uint32_t made_default = 0;
    AtriumTestItemV1* poked = nullptr;
    bool poked_null = false;
    AtriumTestItemV1* other_as_item = reinterpret_cast<AtriumTestItemV1*>(1);
    wl_resource* other = nullptr;
    int pings = 0;
    h.setup = [&](AtriumTestManagerV1* m) {
        m->on_make_item([&](AtriumTestManagerV1* self, uint32_t id, uint32_t def) {
            auto* item = wl::make<AtriumTestItemV1>(self->client(), self->version(), id);
            item->on_ping([&](AtriumTestItemV1* it) {
                ++pings;
                it->send_pong(uint32_t(pings));
            });
            made.push_back(item);
            made_default = def;
            self->send_item_made(item);
        });
        m->on_poke([&](AtriumTestManagerV1*, AtriumTestItemV1* item, wl_resource* o) {
            if (item)
                poked = item;
            else
                poked_null = true;
            other = o;
            other_as_item = AtriumTestItemV1::from(o);
        });
    };
    ASSERT_NE(h.bind(&kListener, &e), nullptr);

    atrium_test_item_v1* item = atrium_test_manager_v1_make_item(h.manager, 42);
    h.pump();
    ASSERT_EQ(made.size(), 1u);
    EXPECT_EQ(made_default, 42u);
    EXPECT_EQ(made[0]->version(), 2u);
    // The event's object is the client's own proxy for the same id.
    EXPECT_EQ(e.item_made, item);

    uint32_t pong = 0;
    static const atrium_test_item_v1_listener il = {
        .pong = [](void* d, atrium_test_item_v1*, uint32_t n) { *static_cast<uint32_t*>(d) = n; },
    };
    atrium_test_item_v1_add_listener(item, &il, &pong);
    atrium_test_item_v1_ping(item);
    atrium_test_item_v1_ping(item);
    h.pump();
    EXPECT_EQ(pings, 2);
    EXPECT_EQ(pong, 2u);

    wl_registry* registry = wl_display_get_registry(h.client);
    atrium_test_manager_v1_poke(h.manager, item, registry);
    h.pump();
    EXPECT_EQ(poked, made[0]);
    EXPECT_NE(other, nullptr);
    // A resource of another kind is not an item.
    EXPECT_EQ(other_as_item, nullptr);

    atrium_test_manager_v1_poke(h.manager, nullptr, nullptr);
    h.pump();
    EXPECT_TRUE(poked_null);
    EXPECT_EQ(other, nullptr);
    EXPECT_EQ(wl_display_get_error(h.client), 0);
    wl_registry_destroy(registry);
    atrium_test_item_v1_destroy(item);
    h.pump();
}

TEST(WlScanner, DestructorRunsTheHandlerThenDeletes) {
    Harness h;
    std::vector<std::string> order;
    wl::Weak<AtriumTestManagerV1> weak;
    h.setup = [&](AtriumTestManagerV1* m) {
        weak = m;
        m->on_destroy([&](AtriumTestManagerV1*) { order.push_back("request"); });
        m->on_gone([&] { order.push_back("gone"); });
    };
    ASSERT_NE(h.bind(), nullptr);
    EXPECT_TRUE(weak);
    atrium_test_manager_v1_destroy(h.manager);
    h.manager = nullptr;
    h.pump();
    EXPECT_EQ(order, (std::vector<std::string>{"request", "gone"}));
    EXPECT_FALSE(weak);
}

TEST(WlScanner, DestructorWithoutHandlerStillDestroys) {
    Harness h;
    wl::Weak<AtriumTestManagerV1> weak;
    h.setup = [&](AtriumTestManagerV1* m) { weak = m; };
    ASSERT_NE(h.bind(), nullptr);
    atrium_test_manager_v1_destroy(h.manager);
    h.manager = nullptr;
    h.pump();
    EXPECT_FALSE(weak);
}

TEST(WlScanner, DisconnectDeletesEverything) {
    Harness h;
    int gone = 0;
    wl::Weak<AtriumTestManagerV1> manager;
    wl::Weak<AtriumTestItemV1> item;
    h.setup = [&](AtriumTestManagerV1* m) {
        manager = m;
        m->on_gone([&] { ++gone; });
        m->on_make_item([&](AtriumTestManagerV1* self, uint32_t id, uint32_t) {
            item = wl::make<AtriumTestItemV1>(self->client(), self->version(), id);
            item->on_gone([&] { ++gone; });
        });
    };
    ASSERT_NE(h.bind(), nullptr);
    atrium_test_item_v1* proxy = atrium_test_manager_v1_make_item(h.manager, 0);
    h.pump();
    ASSERT_TRUE(item);
    wl_client_destroy(h.peer);
    EXPECT_EQ(gone, 2);
    EXPECT_FALSE(manager);
    EXPECT_FALSE(item);
    atrium_test_item_v1_destroy(proxy);
    atrium_test_manager_v1_destroy(h.manager);
    wl_display_disconnect(h.client);
    h.client = nullptr;
    h.manager = nullptr;
}

TEST(WlScanner, CompositorSideDestroy) {
    Harness h;
    bool gone = false;
    AtriumTestManagerV1* m = nullptr;
    h.setup = [&](AtriumTestManagerV1* x) {
        m = x;
        x->on_gone([&] { gone = true; });
    };
    ASSERT_NE(h.bind(), nullptr);
    wl::Weak<AtriumTestManagerV1> weak = m;
    m->destroy();
    EXPECT_TRUE(gone);
    EXPECT_FALSE(weak);
}

TEST(WlScanner, DetachedObjectsIgnoreRequestsButStillGo) {
    Harness h;
    int scalars = 0;
    bool gone = false;
    wl::Weak<AtriumTestManagerV1> weak;
    h.setup = [&](AtriumTestManagerV1* m) {
        weak = m;
        m->on_scalars([&](AtriumTestManagerV1*, int32_t, uint32_t, double) { ++scalars; });
        m->on_gone([&] { gone = true; });
    };
    ASSERT_NE(h.bind(), nullptr);
    weak->detach();
    EXPECT_TRUE(weak->inert());
    atrium_test_manager_v1_scalars(h.manager, 1, 1, 0);
    atrium_test_manager_v1_destroy(h.manager);
    h.manager = nullptr;
    h.pump();
    EXPECT_EQ(scalars, 0);
    EXPECT_FALSE(gone);  // detach dropped on_gone too
    EXPECT_FALSE(weak);
}

TEST(WlScanner, DetachedObjectsStillMakeWhatTheClientAsksFor) {
    Harness h;
    wl::Weak<AtriumTestManagerV1> weak;
    h.setup = [&](AtriumTestManagerV1* m) { weak = m; };
    ASSERT_NE(h.bind(), nullptr);
    weak->detach();
    // Using the new object would be a protocol error if it didn't exist.
    atrium_test_item_v1* item = atrium_test_manager_v1_make_item(h.manager, 0);
    atrium_test_item_v1_ping(item);
    atrium_test_item_v1_destroy(item);
    h.pump();
    EXPECT_EQ(wl_display_get_error(h.client), 0);
}
