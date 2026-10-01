#pragma once
// A libwayland server and client in one process over a socketpair, for
// testing the protocol layer (src/wl) against a real client.
#include <wayland-client.h>
#include <wayland-server-core.h>

#include <poll.h>
#include <sys/socket.h>

#include <cerrno>
#include <cstring>
#include <string>
#include <vector>

namespace wltest {

struct Harness {
    wl_display* server = wl_display_create();
    wl_display* client = nullptr;
    wl_client* peer = nullptr;
    wl_registry* registry = nullptr;
    struct Advertised {
        uint32_t name;
        std::string interface;
        uint32_t version;
    };
    std::vector<Advertised> globals;

    Harness() {
        int fds[2];
        socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds);
        peer = wl_client_create(server, fds[0]);
        client = wl_display_connect_to_fd(fds[1]);
    }

    ~Harness() { destroy_server(); }

    // The client goes; the server tears down what it had.
    void disconnect() {
        if (registry)
            wl_registry_destroy(registry);
        registry = nullptr;
        if (client)
            wl_display_disconnect(client);
        client = nullptr;
        if (server) {
            pump_server();
            wl_display_destroy_clients(server);
        }
    }

    void destroy_server() {
        disconnect();
        if (server)
            wl_display_destroy(server);
        server = nullptr;
    }

    void pump_server() {
        wl_event_loop_dispatch(wl_display_get_event_loop(server), 0);
        wl_display_flush_clients(server);
    }

    // Moves every queued message both ways until both sides go quiet.
    // Stops at a protocol error (error() says which).
    void pump() {
        if (!client)
            return;
        for (int i = 0; i < 8; ++i) {
            wl_display_flush(client);
            pump_server();
            while (wl_display_prepare_read(client) != 0)
                if (wl_display_dispatch_pending(client) < 0)
                    return;
            pollfd p{wl_display_get_fd(client), POLLIN, 0};
            if (poll(&p, 1, 0) > 0)
                wl_display_read_events(client);
            else
                wl_display_cancel_read(client);
            if (wl_display_dispatch_pending(client) < 0)
                return;
        }
    }

    int error() const { return client ? wl_display_get_error(client) : 0; }
    // The protocol error's code, when error() is EPROTO.
    uint32_t protocol_error() const {
        const wl_interface* iface = nullptr;
        uint32_t id = 0;
        return wl_display_get_protocol_error(client, &iface, &id);
    }
    // Whether the client was killed by `code` on an object of `interface`:
    // unlike protocol_error() alone, true only if an error was posted (many
    // codes are 0). An object the client already destroyed (an error on a
    // destructor request) comes back without its interface.
    bool posted(const char* interface, uint32_t code) const {
        if (!client || wl_display_get_error(client) != EPROTO)
            return false;
        const wl_interface* iface = nullptr;
        uint32_t id = 0;
        const uint32_t got = wl_display_get_protocol_error(client, &iface, &id);
        return got == code && (!iface || std::strcmp(iface->name, interface) == 0);
    }

    // Binds `interface` at `version` (the advertised one when 0).
    template <class T>
    T* bind(const wl_interface* interface, uint32_t version = 0) {
        if (!registry) {
            registry = wl_display_get_registry(client);
            static const wl_registry_listener rl = {
                .global =
                    [](void* d, wl_registry*, uint32_t name, const char* iface, uint32_t v) {
                        static_cast<Harness*>(d)->globals.push_back({name, iface, v});
                    },
                .global_remove =
                    [](void* d, wl_registry*, uint32_t name) {
                        std::erase_if(static_cast<Harness*>(d)->globals,
                                      [name](const Advertised& a) { return a.name == name; });
                    },
            };
            wl_registry_add_listener(registry, &rl, this);
        }
        pump();
        for (const Advertised& g : globals)
            if (g.interface == interface->name)
                return static_cast<T*>(wl_registry_bind(registry, g.name, interface, version ? version : g.version));
        return nullptr;
    }
};

} // namespace wltest
