// xwayland-keyboard-grab-v1: Xwayland asks it while an X11 app grabs the
// keyboard (virt-manager, remote desktops, games under X11), so that the
// keys go to that app and not to the desktop's shortcuts. Specced from KWin
// (wayland/xwaylandkeyboardgrab_v1.cpp, WaylandServer::
// isKeyboardShortcutsInhibited): a grab on the focused surface counts as a
// keyboard-shortcuts inhibitor. Only Xwayland gets to ask (window_hints.cpp).
#include "seat.hpp"
#include "server.hpp"

#include "xwayland-keyboard-grab-unstable-v1-protocol.h"

#include <algorithm>

namespace atrium {

namespace {

struct Grab {
    Seat* seat;
    wl_resource* resource;
    wlr_surface* surface;
    wl_listener surface_gone{};
};

std::vector<Grab*>& grabs() {
    static std::vector<Grab*> all;
    return all;
}

void forget(Grab* g) {
    if (g->surface)
        wl_list_remove(&g->surface_gone.link);
    std::erase(grabs(), g);
    delete g;
}

const struct zwp_xwayland_keyboard_grab_v1_interface grab_impl = {
    .destroy = [](wl_client*, wl_resource* r) { wl_resource_destroy(r); },
};

const struct zwp_xwayland_keyboard_grab_manager_v1_interface manager_impl = {
    .destroy = [](wl_client*, wl_resource* r) { wl_resource_destroy(r); },
    .grab_keyboard =
        [](wl_client* client, wl_resource* manager, uint32_t id, wl_resource* surface, wl_resource*) {
            auto* seat = static_cast<Seat*>(wl_resource_get_user_data(manager));
            wl_resource* r = wl_resource_create(client, &zwp_xwayland_keyboard_grab_v1_interface,
                                                wl_resource_get_version(manager), id);
            if (!r) {
                wl_client_post_no_memory(client);
                return;
            }
            auto* g = new Grab{seat, r, wlr_surface_from_resource(surface)};
            g->surface_gone.notify = [](wl_listener* l, void*) {
                Grab* self = wl_container_of(l, self, surface_gone);
                wl_list_remove(&self->surface_gone.link);
                self->surface = nullptr;
            };
            wl_signal_add(&g->surface->events.destroy, &g->surface_gone);
            grabs().push_back(g);
            wl_resource_set_implementation(r, &grab_impl, g, [](wl_resource* res) {
                if (auto* g = static_cast<Grab*>(wl_resource_get_user_data(res)))
                    forget(g);
            });
        },
};

} // namespace

bool Seat::xwayland_grabbed(wlr_surface* surface) const {
    return std::ranges::any_of(grabs(), [&](const Grab* g) { return g->seat == this && g->surface == surface; });
}

void Seat::create_xwayland_grab() {
    xwayland_grab_global = wl_global_create(server.display, &zwp_xwayland_keyboard_grab_manager_v1_interface, 1, this,
        [](wl_client* client, void* data, uint32_t version, uint32_t id) {
            wl_resource* r = wl_resource_create(client, &zwp_xwayland_keyboard_grab_manager_v1_interface, int(version), id);
            if (!r) {
                wl_client_post_no_memory(client);
                return;
            }
            wl_resource_set_implementation(r, &manager_impl, data, nullptr);
        });
}

Seat::XwaylandGrabs::~XwaylandGrabs() {
    // The seat goes before the display: grabs left are cut loose.
    for (Grab* g : std::vector<Grab*>(grabs())) {
        wl_resource_set_user_data(g->resource, nullptr);
        wl_resource_set_destructor(g->resource, nullptr);
        forget(g);
    }
}

} // namespace atrium
