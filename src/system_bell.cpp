#include "system_bell.hpp"

#include "server.hpp"
#include "view.hpp"

#include "xdg-system-bell-v1-server.hpp"

#include <ctime>

namespace atrium {

SystemBell::SystemBell(Server& server) : server_(server) {
    using wl::XdgSystemBellV1;
    global_ = std::make_unique<wl::Global>(
        server.display, XdgSystemBellV1::interface(), XdgSystemBellV1::kVersion,
        [this](wl_client* client, uint32_t version, uint32_t id) {
            auto* bell = wl::make<XdgSystemBellV1>(client, version, id);
            if (!bell)
                return;
            bell->on_ring([this](XdgSystemBellV1*, wl_resource* surface) { ring(surface); });
            std::erase_if(bells_, [](const wl::Weak<XdgSystemBellV1>& b) { return !b; });
            bells_.push_back(bell);
        });
}

// Bells already bound outlive the global by a moment: their handlers point
// here, so they go quiet with it.
SystemBell::~SystemBell() {
    global_.reset();
    for (wl::Weak<wl::XdgSystemBellV1>& b : bells_)
        if (b)
            b->detach();
}

void SystemBell::ring(wl_resource* surface) {
    if (surface)
        if (View* v = Server::owner_of(wlr_surface_from_resource(surface)).view;
            v && v != server_.focused_view && !v->urgent) {
            v->urgent = true;
            server_.notify_window(*v, "changed");
        }
    if (!server_.config.system_bell)
        return;
    // A held key can ring many times a second; one sound is plenty.
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    int64_t now = int64_t(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
    if (now - last_sound_ms_ < 150)
        return;
    last_sound_ms_ = now;
    server_.spawn("canberra-gtk-play -i bell -d atrium 2>/dev/null || "
                  "pw-play /usr/share/sounds/freedesktop/stereo/bell.oga");
}

} // namespace atrium
