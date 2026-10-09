#pragma once
// Remote control and input capture, as KWin's eis plugin does them: atrium
// holds the EIS (emulated input server) end of libei itself.
//
// Remote control (the RemoteDesktop portal): an app allowed to drive the
// seat gets its own pointer, keyboard and touchscreen, which atrium's input
// handling takes as any other device. It sends through libei (ConnectToEIS)
// or the portal's Notify* calls (here as remote.input).
//
// Input capture (the InputCapture portal, Deskflow and Input Leap): the app
// sets barriers along the screens' outer edges; pushing the pointer through
// one hands the pointer and keyboard to the app (it sees them through libei)
// until it lets go, or Super+Shift+Escape takes them back.
//
// The portal asks for both over atrium's IPC; a session ends with the IPC
// connection that made it.

#include "eis_core.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <vector>

struct eis;
struct eis_client;
struct eis_device;
struct eis_event;
struct eis_seat;
struct wl_event_source;
struct wlr_keyboard;
struct wlr_pointer;
struct wlr_pointer_axis_event;
struct wlr_touch;

namespace atrium {

class Server;

class Eis {
public:
    explicit Eis(Server& server);
    ~Eis();
    Eis(const Eis&) = delete;
    Eis& operator=(const Eis&) = delete;

    // A remote control session for `owner` (an IPC client) with the
    // portal's device bits; its cookie.
    uint32_t remote_start(const void* owner, uint32_t devices);
    // Portal Notify* input for a session; false when it has no such device.
    bool remote_input(uint32_t cookie, const nlohmann::json& event);
    // An input capture session; its cookie.
    uint32_t capture_create(const void* owner, uint32_t devices);
    bool capture_barriers(uint32_t cookie, const std::vector<eis::Barrier>& barriers);
    bool capture_enable(uint32_t cookie, bool on);
    bool capture_release(uint32_t cookie, std::optional<std::pair<double, double>> at);
    // A libei client's end of a session's EIS socket (-1: no such session).
    int connect(uint32_t cookie);
    void close(uint32_t cookie);
    void drop_owner(const void* owner);

    // The seat's input while a capture holds it: true when taken.
    bool motion(double x, double y, double dx, double dy, double udx, double udy);
    bool button(uint32_t button, bool pressed);
    bool axis(const wlr_pointer_axis_event& e);
    bool key(uint32_t keycode, bool pressed, uint32_t mods, uint32_t sym);
    bool capturing() const { return active_ != nullptr; }
    // The screens or the keymap changed: devices offered anew.
    void outputs_changed();
    void keymap_changed();

    struct Session;

private:
    Session* find(uint32_t cookie) const;
    void dispatch(Session& s);
    void handle(Session& s, eis_event* e);
    void bind_seat(Session& s, eis_event* e);
    void add_devices(Session& s);
    void remove_devices(Session& s);
    eis_device* new_device(Session& s, const char* name);
    eis_device* absolute_device(Session& s);
    eis_device* keyboard_device(Session& s);
    void ensure_virtual(Session& s);
    void activate(Session& s, uint32_t barrier, double x, double y);
    void deactivate(std::optional<std::pair<double, double>> at);
    void normalize(double lx, double ly, double& nx, double& ny) const;
    uint64_t now_us() const;
    int keymap_fd(size_t& size) const;

    Server& server_;
    std::map<uint32_t, std::unique_ptr<Session>> sessions_;
    uint32_t next_cookie_ = 0;
    Session* active_ = nullptr;
};

} // namespace atrium
