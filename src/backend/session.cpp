#include "backend/session.hpp"

extern "C" {
#include <libseat.h>
#include <libudev.h>
#include <wayland-server-core.h>
#include <wlr/util/log.h>
}

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>
#include <xf86drm.h>

#include <algorithm>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <string_view>

namespace atrium::backend {

namespace {

void seat_log(libseat_log_level level, const char* fmt, va_list args) {
    const wlr_log_importance imp = level == LIBSEAT_LOG_LEVEL_ERROR  ? WLR_ERROR
                                   : level == LIBSEAT_LOG_LEVEL_INFO ? WLR_INFO
                                                                     : WLR_DEBUG;
    char line[512];
    std::vsnprintf(line, sizeof(line), fmt, args);
    wlr_log(imp, "[libseat] %s", line);
}

bool is_card(const char* sysname) {
    // card0, card1: not card0-DP-1 (a connector) nor renderD128.
    if (!sysname || std::strncmp(sysname, "card", 4) != 0)
        return false;
    for (const char* p = sysname + 4; *p; ++p)
        if (*p < '0' || *p > '9')
            return false;
    return sysname[4] != '\0';
}

} // namespace

// Reaches the session's privates from libseat's C callbacks.
struct SessionAccess {
    static void enable(libseat*, void* data) {
        auto* s = static_cast<Session*>(data);
        s->active_ = true;
        s->events.active.emit(true);
    }
    static void disable(libseat* seat, void* data) {
        auto* s = static_cast<Session*>(data);
        s->active_ = false;
        s->events.active.emit(false);  // everything lets go of its devices first
        libseat_disable_seat(seat);
    }
};

std::unique_ptr<Session> Session::create(wl_event_loop* loop) {
    std::unique_ptr<Session> s(new Session());
    s->loop_ = loop;
    libseat_set_log_handler(seat_log);
    libseat_set_log_level(LIBSEAT_LOG_LEVEL_INFO);
    static const libseat_seat_listener kListener = {
        .enable_seat = SessionAccess::enable,
        .disable_seat = SessionAccess::disable,
    };
    s->seat_handle_ = libseat_open_seat(&kListener, s.get());
    if (!s->seat_handle_) {
        wlr_log(WLR_ERROR, "session: no seat to take (logind or seatd)");
        return nullptr;
    }
    // The seat is ours once libseat says so: wait for it.
    while (!s->active_)
        if (libseat_dispatch(s->seat_handle_, -1) < 0) {
            wlr_log(WLR_ERROR, "session: the seat never became ours");
            return nullptr;
        }
    const char* name = libseat_seat_name(s->seat_handle_);
    s->seat_ = name ? name : "seat0";

    s->seat_source_ = wl_event_loop_add_fd(
        loop, libseat_get_fd(s->seat_handle_), WL_EVENT_READABLE,
        [](int, uint32_t, void* data) {
            auto* self = static_cast<Session*>(data);
            if (libseat_dispatch(self->seat_handle_, 0) < 0)
                wlr_log(WLR_ERROR, "session: libseat dispatch failed");
            return 1;
        },
        s.get());

    s->udev_ = udev_new();
    if (!s->udev_)
        return nullptr;
    s->monitor_ = udev_monitor_new_from_netlink(s->udev_, "udev");
    if (s->monitor_) {
        udev_monitor_filter_add_match_subsystem_devtype(s->monitor_, "drm", nullptr);
        udev_monitor_enable_receiving(s->monitor_);
        s->udev_source_ = wl_event_loop_add_fd(
            loop, udev_monitor_get_fd(s->monitor_), WL_EVENT_READABLE,
            [](int, uint32_t, void* data) {
                static_cast<Session*>(data)->dispatch_udev();
                return 1;
            },
            s.get());
    }
    wlr_log(WLR_INFO, "session: on %s", s->seat_.c_str());
    return s;
}

Session::~Session() {
    for (auto& d : devices_)
        if (seat_handle_)
            libseat_close_device(seat_handle_, d->id);
    devices_.clear();
    if (udev_source_)
        wl_event_source_remove(udev_source_);
    if (seat_source_)
        wl_event_source_remove(seat_source_);
    if (monitor_)
        udev_monitor_unref(monitor_);
    if (udev_)
        udev_unref(udev_);
    if (seat_handle_)
        libseat_close_seat(seat_handle_);
}

Session::Device* Session::open(const std::string& path) {
    int fd = -1;
    const int id = libseat_open_device(seat_handle_, path.c_str(), &fd);
    if (id < 0) {
        wlr_log(WLR_ERROR, "session: couldn't open %s: %s", path.c_str(), std::strerror(errno));
        return nullptr;
    }
    struct stat st {};
    if (fstat(fd, &st) < 0) {
        libseat_close_device(seat_handle_, id);
        return nullptr;
    }
    auto d = std::make_unique<Device>();
    d->fd = fd;
    d->id = id;
    d->dev = st.st_rdev;
    d->path = path;
    devices_.push_back(std::move(d));
    return devices_.back().get();
}

void Session::close(Device* d) {
    if (!d)
        return;
    if (libseat_close_device(seat_handle_, d->id) < 0)
        wlr_log(WLR_ERROR, "session: couldn't close %s", d->path.c_str());
    std::erase_if(devices_, [d](const auto& x) { return x.get() == d; });
}

Session::Device* Session::device_for_fd(int fd) const {
    for (const auto& d : devices_)
        if (d->fd == fd)
            return d.get();
    return nullptr;
}

bool Session::change_vt(unsigned vt) {
    return seat_handle_ && libseat_switch_session(seat_handle_, int(vt)) == 0;
}

void Session::dispatch_udev() {
    udev_device* dev = udev_monitor_receive_device(monitor_);
    if (!dev)
        return;
    const char* sysname = udev_device_get_sysname(dev);
    const char* devnode = udev_device_get_devnode(dev);
    const char* action = udev_device_get_action(dev);
    if (!is_card(sysname) || !devnode || !action) {
        udev_device_unref(dev);
        return;
    }
    // Only this seat's.
    const char* seat = udev_device_get_property_value(dev, "ID_SEAT");
    if ((seat ? std::string_view(seat) : std::string_view("seat0")) != seat_) {
        udev_device_unref(dev);
        return;
    }
    const dev_t num = udev_device_get_devnum(dev);
    Device* known = nullptr;
    for (const auto& d : devices_)
        if (d->dev == num)
            known = d.get();
    const std::string_view act = action;
    if (act == "add" && !known) {
        events.add_gpu.emit(devnode);
    } else if (act == "change" && known) {
        Device::Change c;
        const char* hotplug = udev_device_get_property_value(dev, "HOTPLUG");
        const char* lease = udev_device_get_property_value(dev, "LEASE");
        if (hotplug && std::string_view(hotplug) == "1") {
            c.type = Device::Change::Type::Hotplug;
            if (const char* v = udev_device_get_property_value(dev, "CONNECTOR"))
                c.connector = uint32_t(std::strtoul(v, nullptr, 10));
            if (const char* v = udev_device_get_property_value(dev, "PROPERTY"))
                c.property = uint32_t(std::strtoul(v, nullptr, 10));
            known->events.change.emit(c);
        } else if (lease && std::string_view(lease) == "1") {
            c.type = Device::Change::Type::Lease;
            known->events.change.emit(c);
        }
    } else if (act == "remove" && known) {
        known->events.remove.emit();
    }
    udev_device_unref(dev);
}

std::vector<std::string> order_gpus(std::vector<GpuCandidate> found, const char* override_list) {
    std::vector<std::string> out;
    if (override_list && *override_list) {
        std::string_view rest = override_list;
        while (!rest.empty()) {
            const size_t colon = rest.find(':');
            const std::string_view part = rest.substr(0, colon);
            if (!part.empty())
                out.emplace_back(part);
            if (colon == std::string_view::npos)
                break;
            rest.remove_prefix(colon + 1);
        }
        return out;
    }
    // The firmware's boot GPU first (it drives the built-in panel); the rest
    // in the order found.
    std::stable_partition(found.begin(), found.end(), [](const GpuCandidate& c) { return c.boot_vga; });
    for (auto& c : found)
        out.push_back(std::move(c.node));
    return out;
}

std::vector<std::string> Session::find_gpus() const {
    const char* env = std::getenv("ATRIUM_DRM_DEVICES");
    if (!env || !*env)
        env = std::getenv("WLR_DRM_DEVICES");
    std::vector<GpuCandidate> found;
    if (!env || !*env) {
        udev_enumerate* en = udev_enumerate_new(udev_);
        udev_enumerate_add_match_subsystem(en, "drm");
        udev_enumerate_add_match_sysname(en, "card[0-9]*");
        udev_enumerate_scan_devices(en);
        udev_list_entry* entry;
        udev_list_entry_foreach(entry, udev_enumerate_get_list_entry(en)) {
            udev_device* dev = udev_device_new_from_syspath(udev_, udev_list_entry_get_name(entry));
            if (!dev)
                continue;
            const char* seat = udev_device_get_property_value(dev, "ID_SEAT");
            const char* node = udev_device_get_devnode(dev);
            if (node && is_card(udev_device_get_sysname(dev)) &&
                (seat ? std::string_view(seat) : std::string_view("seat0")) == seat_) {
                GpuCandidate c{node, false};
                if (udev_device* pci = udev_device_get_parent_with_subsystem_devtype(dev, "pci", nullptr)) {
                    const char* boot = udev_device_get_sysattr_value(pci, "boot_vga");
                    c.boot_vga = boot && std::string_view(boot) == "1";
                }
                found.push_back(std::move(c));
            }
            udev_device_unref(dev);
        }
        udev_enumerate_unref(en);
    }
    return order_gpus(std::move(found), env);
}

} // namespace atrium::backend
