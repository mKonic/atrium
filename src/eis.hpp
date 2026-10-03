#pragma once
// Remote input over libei: an app the RemoteDesktop portal let control the
// desktop (a remote-desktop server, a KVM switch) sends pointer, keyboard
// and touch input to its session's EIS socket, and atrium feeds it to the
// seat as it does a virtual keyboard's and pointer's. atrium-portal opens a
// session's socket (IPC eis.open) with the devices the user allowed, hands
// the app a connection to it, and closes it when the session ends (or its
// IPC connection does).

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace atrium {

class Server;

class Eis {
public:
    explicit Eis(Server& server);
    ~Eis();
    Eis(const Eis&) = delete;
    Eis& operator=(const Eis&) = delete;

    // The devices a session may have, as the portal numbers them.
    enum Devices : uint32_t { Keyboard = 1, Pointer = 2, Touch = 4 };

    struct Opened {
        uint64_t id;
        std::string path;  // the socket to connect to
    };
    // A session's socket, for `devices`, owned by IPC client `owner`.
    std::optional<Opened> open(uint32_t devices, uint64_t owner);
    bool close(uint64_t id);
    void close_owned(uint64_t owner);
    // The screens changed: absolute pointers and touch follow them.
    void outputs_changed();

private:
    struct Session;
    struct Bound;
    void dispatch(Session& s);
    void make_devices(Session& s, Bound& b);
    void drop_devices(Bound& b);

    Server& server_;
    std::vector<std::unique_ptr<Session>> sessions_;
    uint64_t next_ = 1;
};

} // namespace atrium
