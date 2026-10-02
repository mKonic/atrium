// xwayland_shell_v1, after wlroots' xwayland/shell.c (MIT).
#include "wl/xwayland_shell.hpp"

#include <algorithm>

namespace atrium::wl {

XwaylandSurface::XwaylandSurface(wl_client* client, uint32_t version, uint32_t id, Surface* surface,
                                 XwaylandShell& shell)
    : XwaylandSurfaceV1(client, version, id), shell_(shell), surface_(surface) {
    surface_gone_ = surface->events.destroy.connect([this] { surface_ = nullptr; });
    on_set_serial([this](XwaylandSurfaceV1*, uint32_t lo, uint32_t hi) {
        if (serial_) {
            post_error(uint32_t(Error::AlreadyAssociated), "the surface already has an X11 serial");
            return;
        }
        serial_ = uint64_t(hi) << 32 | lo;
    });
}

XwaylandSurface::~XwaylandSurface() {
    if (surface_)
        surface_->clear_role(this);
}

void XwaylandSurface::commit(Surface&) {
    if (serial_ && !announced_) {
        announced_ = true;
        shell_.new_surface.emit(this);
    }
}

XwaylandShell::XwaylandShell(wl_display* display) {
    global_ = Global::create<XwaylandShellV1>(display, 1, [this](wl_client* client, uint32_t version, uint32_t id) {
        if (client != client_) {
            wl_client_post_implementation_error(client, "xwayland_shell_v1 is for Xwayland only");
            return;
        }
        auto* shell = make<XwaylandShellV1>(client, version, id);
        if (!shell)
            return;
        shell->on_get_xwayland_surface([this](XwaylandShellV1* self, uint32_t id, wl_resource* surface_res) {
            Surface* surface = Surface::from(surface_res);
            if (!surface)
                return;
            auto* xs = make<XwaylandSurface>(self->client(), self->version(), id, surface, *this);
            if (!xs)
                return;
            if (!surface->set_role(xs, self, uint32_t(XwaylandShellV1::Error::Role))) {
                xs->detach();
                return;
            }
            std::erase_if(surfaces_, [](const auto& w) { return !w; });
            surfaces_.emplace_back(xs);
        });
    });
}

XwaylandShell::~XwaylandShell() {
    for (auto& w : surfaces_)
        if (w)
            w->detach();
}

Surface* XwaylandShell::surface_from_serial(uint64_t serial) const {
    for (const auto& w : surfaces_)
        if (w && w->serial() == serial && w->surface())
            return w->surface();
    return nullptr;
}

} // namespace atrium::wl
