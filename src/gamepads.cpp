#include "gamepads.hpp"

#include "server.hpp"

#include <libudev.h>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <sys/ioctl.h>
#include <unistd.h>

namespace atrium {

namespace {

constexpr size_t kBitsPerLong = sizeof(unsigned long) * 8;

} // namespace

Gamepads::Gamepads(Server& server) : server_(server) {
    udev_ = udev_new();
    if (!udev_)
        return;
    monitor_ = udev_monitor_new_from_netlink(udev_, "udev");
    if (monitor_) {
        udev_monitor_filter_add_match_subsystem_devtype(monitor_, "input", nullptr);
        udev_monitor_enable_receiving(monitor_);
        monitor_source_ = wl_event_loop_add_fd(server.loop, udev_monitor_get_fd(monitor_), WL_EVENT_READABLE,
            [](int, uint32_t, void* data) {
                static_cast<Gamepads*>(data)->hotplug();
                return 0;
            }, this);
    }
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator("/dev/input", ec))
        if (e.path().filename().string().starts_with("event"))
            add(e.path().string());
}

Gamepads::~Gamepads() {
    for (auto& [node, pad] : pads_) {
        wl_event_source_remove(pad.source);
        close(pad.fd);
    }
    if (monitor_source_)
        wl_event_source_remove(monitor_source_);
    if (monitor_)
        udev_monitor_unref(monitor_);
    if (udev_)
        udev_unref(udev_);
}

void Gamepads::hotplug() {
    while (udev_device* d = udev_monitor_receive_device(monitor_)) {
        const char* node = udev_device_get_devnode(d);
        const char* action = udev_device_get_action(d);
        if (node && action && std::string_view(node).starts_with("/dev/input/event")) {
            if (std::strcmp(action, "add") == 0)
                add(node);
            else if (std::strcmp(action, "remove") == 0)
                remove(node);
        }
        udev_device_unref(d);
    }
}

void Gamepads::add(const std::string& node) {
    if (pads_.contains(node))
        return;
    // Only what libinput doesn't take (KWin's isHandledByLibinput).
    const std::string sysname = std::filesystem::path(node).filename().string();
    udev_device* d = udev_device_new_from_subsystem_sysname(udev_, "input", sysname.c_str());
    if (!d)
        return;
    const char* joystick = udev_device_get_property_value(d, "ID_INPUT_JOYSTICK");
    const bool pad = joystick && std::strcmp(joystick, "1") == 0;
    udev_device_unref(d);
    if (!pad)
        return;
    // Pads are open to the user without logind, as games open them.
    const int fd = open(node.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0)
        return;
    unsigned long abs[ABS_CNT / kBitsPerLong + 1] = {}, key[KEY_CNT / kBitsPerLong + 1] = {};
    if (ioctl(fd, EVIOCGBIT(EV_ABS, sizeof abs), abs) < 0 || ioctl(fd, EVIOCGBIT(EV_KEY, sizeof key), key) < 0 ||
        !is_game_controller(abs, key)) {
        close(fd);
        return;
    }
    wl_event_source* source = wl_event_loop_add_fd(server_.loop, fd, WL_EVENT_READABLE,
        [](int fd, uint32_t mask, void* data) {
            auto* self = static_cast<Gamepads*>(data);
            if (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) {
                // Unplugged: udev's remove follows and tidies up.
                for (auto& [node, pad] : self->pads_)
                    if (pad.fd == fd) {
                        self->remove(node);
                        break;
                    }
                return 0;
            }
            self->read_pad(fd);
            return 0;
        }, this);
    pads_[node] = {fd, source};
    char name[128] = "?";
    ioctl(fd, EVIOCGNAME(sizeof name), name);
    wlr_log(WLR_INFO, "gamepad: %s (%s) counts as activity", name, node.c_str());
}

void Gamepads::remove(const std::string& node) {
    const auto it = pads_.find(node);
    if (it == pads_.end())
        return;
    wl_event_source_remove(it->second.source);
    close(it->second.fd);
    pads_.erase(it);
}

void Gamepads::read_pad(int fd) {
    input_event ev[64];
    bool any = false;
    ssize_t n;
    while ((n = read(fd, ev, sizeof ev)) > 0)
        any = true;
    if (any)
        server_.note_activity();  // KWin's simulateUserActivity, per event
}

} // namespace atrium
