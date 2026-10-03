#pragma once
// Remote input over libei: an app the RemoteDesktop portal let control the
// desktop (a remote-desktop server, a KVM switch) sends pointer, keyboard
// and touch input to its session's EIS socket, and atrium feeds it to the
// seat as it does a virtual keyboard's and pointer's. atrium-portal opens a
// session's socket (IPC eis.open) with the devices the user allowed, hands
// the app a connection to it, and closes it when the session ends (or its
// IPC connection does).
//
// Input capture (Deskflow, Input Leap) is the other way round: when the
// pointer goes through one of a session's barriers on the desktop's edge,
// atrium hides it and sends the pointer, buttons, scrolling and keys to
// the session's clients instead, until the app hands them back (or
// Super+Escape takes them back).

#include "input_capture_core.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

struct eis_device;

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
    // What an input capture session hears: capture began (through `barrier`,
    // the pointer at x, y) or ended, or it was turned off.
    struct CaptureEvent {
        enum Kind { Activated, Deactivated, Disabled } kind;
        uint32_t activation = 0;
        double x = 0, y = 0;
        uint32_t barrier = 0;
    };
    using CaptureListener = std::function<void(const CaptureEvent&)>;

    // A session's socket, for `devices`, owned by IPC client `owner`. With a
    // listener it's an input capture session: its clients receive the
    // input atrium takes when the pointer goes through one of its barriers.
    std::optional<Opened> open(uint32_t devices, uint64_t owner, CaptureListener capture = {});
    bool close(uint64_t id);
    void close_owned(uint64_t owner);
    // The screens changed: absolute pointers and touch follow them.
    void outputs_changed();

    // Input capture: its barriers (the ids of those refused), turning it on
    // and off, and handing the input back (the pointer put at `to`).
    std::vector<uint32_t> set_barriers(uint64_t id, std::vector<input_capture::Barrier> barriers);
    bool enable(uint64_t id);
    bool disable(uint64_t id);
    bool release(uint64_t id, std::optional<std::pair<double, double>> to);

    // The seat's input, taken while a capture is active (true: taken).
    bool capture_motion(uint32_t time, double x, double y, double dx, double dy);
    bool capture_button(uint32_t time, uint32_t button, bool pressed);
    bool capture_axis(uint32_t time, uint32_t orientation, double delta, int32_t value120);
    bool capture_key(uint32_t time, uint32_t keycode, bool pressed);

private:
    struct Session;
    struct Bound;
    void dispatch(Session& s);
    void make_devices(Session& s, Bound& b);
    void drop_devices(Bound& b);
    Session* find(uint64_t id) const;
    Session* active() const;
    void activate(Session& s, double x, double y, uint32_t barrier);
    void deactivate(Session& s);
    // The devices of an active capture, emulating, with `send` on each that has `cap`.
    void to_devices(Session& s, int cap, const std::function<void(eis_device*)>& send);

    Server& server_;
    std::vector<std::unique_ptr<Session>> sessions_;
    uint64_t next_ = 1;
};

} // namespace atrium
