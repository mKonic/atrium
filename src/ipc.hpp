#pragma once
#include "settings.hpp"

#include <list>
#include <set>
#include <string>

struct wl_event_source;

namespace atrium {

class Server;
class View;

// Bumped only when an existing request or event changes shape; new requests
// and new fields do not bump it.
constexpr int kIpcProtocol = 1;

// The control socket: newline-delimited JSON over a Unix socket at
// $XDG_RUNTIME_DIR/atrium.<WAYLAND_DISPLAY>.sock (exported as ATRIUM_SOCKET).
//
//   → {"id": 1, "cmd": "windows"}
//   ← {"id": 1, "ok": true, "result": [...]}
//   → {"id": 2, "cmd": "window.close", "window": 7}
//   → {"cmd": "subscribe", "topics": ["windows", "settings"]}
//   ← {"event": "window.opened", "window": {...}}
//
// Clients are never waited on: output is buffered per client and a client
// that stops reading is dropped once its buffer grows past a limit.
class Ipc {
public:
    Ipc(Server& server, const std::string& wayland_display);
    ~Ipc();
    Ipc(const Ipc&) = delete;
    Ipc& operator=(const Ipc&) = delete;

    bool ok() const { return listen_fd_ >= 0; }
    const std::string& path() const { return path_; }

    // Send an event to every client subscribed to `topic`.
    void broadcast(const std::string& topic, const json& event);

    static json window_json(const View& view);
    static json spaces_json(const Server& server);
    // {layouts: [{name, code}], active}
    static json keyboard_json(const Server& server);
    static json devices_json(const Server& server);

private:
    struct Client {
        Ipc* owner = nullptr;
        bool dead = false;  // dropped; erased by reap() once no handler uses it
        int fd = -1;
        wl_event_source* source = nullptr;
        std::string in, out;
        std::set<std::string> topics;
    };

    static int on_accept(int fd, uint32_t mask, void* data);
    static int on_client(int fd, uint32_t mask, void* data);
    void accept_client();
    bool read_client(Client& c);
    bool flush(Client& c);
    void send(Client& c, const json& msg);
    void drop(Client& c);
    void reap();

    json handle(Client& c, const json& request);

    Server& server_;
    std::string path_;
    int listen_fd_ = -1;
    wl_event_source* listen_source_ = nullptr;
    std::list<Client> clients_;
};

} // namespace atrium
