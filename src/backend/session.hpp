#pragma once
// The seat atrium runs on: libseat (logind or seatd) opens the devices and
// says when another VT takes them; udev finds the GPUs and reports screens
// plugged in or out. After wlroots' backend/session and aquamarine's
// Session.cpp.
#include "wl/signal.hpp"

#include <sys/types.h>

#include <cstdint>

#include <memory>
#include <string>
#include <vector>

struct libseat;
struct udev;
struct udev_monitor;
struct wl_event_loop;
struct wl_event_source;

namespace atrium::backend {

class Session {
public:
    // A device node opened through the seat (a GPU, an input device).
    struct Device {
        int fd = -1;
        int id = -1;  // libseat's
        dev_t dev = 0;
        std::string path;
        // A DRM device's change uevent: screens plugged in or out (connector
        // and property ids where the kernel names them), or a lease ended.
        struct Change {
            enum class Type { Hotplug, Lease } type = Type::Hotplug;
            uint32_t connector = 0, property = 0;
        };
        struct {
            wl::Signal<const Change&> change;
            wl::Signal<> remove;
        } events;
    };

    // On the seat this process may take; null if there is none.
    static std::unique_ptr<Session> create(wl_event_loop* loop);
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    bool active() const { return active_; }
    const std::string& seat() const { return seat_; }
    struct udev* udev() const { return udev_; }

    Device* open(const std::string& path);
    void close(Device* device);
    Device* device_for_fd(int fd) const;
    // To another VT (Ctrl+Alt+Fn).
    bool change_vt(unsigned vt);

    // The GPUs with display outputs on this seat, the one the firmware booted
    // on first; ATRIUM_DRM_DEVICES (or WLR_DRM_DEVICES), ':'-separated, overrides.
    std::vector<std::string> find_gpus() const;

    struct {
        wl::Signal<bool> active;               // our VT came back (true) or went
        wl::Signal<const std::string&> add_gpu;  // a GPU appeared (an eGPU, a dock)
    } events;

private:
    Session() = default;
    void dispatch_udev();

    wl_event_loop* loop_ = nullptr;
    libseat* seat_handle_ = nullptr;
    struct udev* udev_ = nullptr;
    udev_monitor* monitor_ = nullptr;
    wl_event_source* seat_source_ = nullptr;
    wl_event_source* udev_source_ = nullptr;
    std::string seat_;
    bool active_ = false;
    std::vector<std::unique_ptr<Device>> devices_;

    friend struct SessionAccess;
};

// What find_gpus decides from: each candidate's node and whether the firmware
// booted on it; `override` is the environment's list, if any. (Split out to
// be testable without udev.)
struct GpuCandidate {
    std::string node;
    bool boot_vga = false;
};
std::vector<std::string> order_gpus(std::vector<GpuCandidate> found, const char* override_list);

} // namespace atrium::backend
