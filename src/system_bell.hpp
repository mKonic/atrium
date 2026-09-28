#pragma once
#include "wl/resource.hpp"

#include <cstdint>
#include <memory>
#include <vector>

namespace atrium {

class Server;
namespace wl {
class XdgSystemBellV1;
}

// xdg-system-bell-v1: an app rings the bell. It plays the sound theme's bell
// (windows.system_bell) and marks a window that isn't focused as wanting
// attention.
class SystemBell {
public:
    explicit SystemBell(Server& server);
    ~SystemBell();

    void ring(wl_resource* surface);

private:
    Server& server_;
    std::unique_ptr<wl::Global> global_;
    std::vector<wl::Weak<wl::XdgSystemBellV1>> bells_;
    int64_t last_sound_ms_ = 0;
};

} // namespace atrium
