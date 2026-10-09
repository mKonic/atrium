#pragma once
// Game controllers count as activity, as KWin's gamecontroller plugin has
// them: a game played with a pad keeps the screen awake. The pads are the
// input nodes udev marks ID_INPUT_JOYSTICK=1 (libinput leaves them alone)
// with sticks, a d-pad or gamepad buttons; they're read alongside the game,
// which still gets every event.
#include "wlr.hpp"

#include <linux/input.h>

#include <map>
#include <string>

struct udev;
struct udev_monitor;

namespace atrium {

class Server;

// KWin's isGameControllerDevice, from the node's EV_ABS and EV_KEY bits:
// analog sticks or a d-pad, or joystick or gamepad buttons.
inline bool is_game_controller(const unsigned long* abs, const unsigned long* key) {
    constexpr unsigned kBits = sizeof(unsigned long) * 8;
    auto has = [](const unsigned long* bits, unsigned code) { return (bits[code / kBits] >> (code % kBits)) & 1UL; };
    return has(abs, ABS_X) || has(abs, ABS_HAT0X) || has(key, BTN_JOYSTICK) || has(key, BTN_GAMEPAD);
}

class Gamepads {
public:
    explicit Gamepads(Server& server);
    ~Gamepads();
    Gamepads(const Gamepads&) = delete;
    Gamepads& operator=(const Gamepads&) = delete;

private:
    struct Pad {
        int fd = -1;
        wl_event_source* source = nullptr;
    };

    void add(const std::string& node);
    void remove(const std::string& node);
    void hotplug();
    void read_pad(int fd);

    Server& server_;
    udev* udev_ = nullptr;
    udev_monitor* monitor_ = nullptr;
    wl_event_source* monitor_source_ = nullptr;
    std::map<std::string, Pad> pads_;
};

} // namespace atrium
