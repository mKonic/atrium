// pointer-warp-v1: an app moves the pointer within its own surface (games,
// SDL3, Wine). Specced from KWin (pointer_input.cpp, wayland/pointerwarp_v1)
// and Hyprland (protocols/PointerWarp.cpp): only the surface the pointer is
// on, with a serial from its pointer, to a point on it; while the pointer
// is locked there, the point is the lock's position hint instead.
#include "layer_surface.hpp"
#include "seat.hpp"
#include "server.hpp"
#include "view.hpp"

#include "pointer-warp-v1-protocol.h"

namespace atrium {

namespace {

const struct wp_pointer_warp_v1_interface warp_impl = {
    .destroy = [](wl_client*, wl_resource* r) { wl_resource_destroy(r); },
    .warp_pointer =
        [](wl_client* client, wl_resource* r, wl_resource* surface, wl_resource* pointer, wl_fixed_t x, wl_fixed_t y,
           uint32_t serial) {
            auto* server = static_cast<Server*>(wl_resource_get_user_data(r));
            wlr_seat_client* seat_client = wlr_seat_client_from_pointer_resource(pointer);
            if (!server || !surface || !seat_client || seat_client->client != client)
                return;
            if (!wlr_seat_client_validate_event_serial(seat_client, serial))
                return;
            server->seat->warp_from_client(wlr_surface_from_resource(surface), wl_fixed_to_double(x),
                                           wl_fixed_to_double(y));
        },
};

} // namespace

void Seat::warp_from_client(wlr_surface* surface, double sx, double sy) {
    if (wlr->pointer_state.focused_surface != surface || mode != Mode::Normal)
        return;
    if (sx < 0 || sy < 0 || sx > surface->current.width || sy > surface->current.height)
        return;
    // Locked there: where the pointer shows when the lock ends (KWin).
    if (active_constraint_ && active_constraint_->surface == surface) {
        if (active_constraint_->type == WLR_POINTER_CONSTRAINT_V1_LOCKED) {
            active_constraint_->current.cursor_hint.enabled = true;
            active_constraint_->current.cursor_hint.x = sx;
            active_constraint_->current.cursor_hint.y = sy;
            return;
        }
    }
    // The surface's place on screen: its window's, plus where it sits among
    // that window's subsurfaces.
    double ox = 0, oy = 0;
    wlr_surface* s = surface;
    while (wlr_subsurface* sub = wlr_subsurface_try_from_wlr_surface(s)) {
        ox += sub->current.x;
        oy += sub->current.y;
        s = sub->parent;
    }
    const Owner owner = Server::owner_of(s);
    if (owner.view) {
        double vx, vy;
        owner.view->surface_origin(vx, vy);
        ox += vx, oy += vy;
    } else if (owner.layer && owner.layer->scene_layer) {
        int lx = 0, ly = 0;
        wlr_scene_node_coords(&owner.layer->scene_layer->tree->node, &lx, &ly);
        ox += lx, oy += ly;
    } else {
        return;
    }
    wlr_cursor_warp(cursor, nullptr, ox + sx, oy + sy);
    refresh_pointer();
}

void Seat::create_pointer_warp() {
    wl_global_create(server.display, &wp_pointer_warp_v1_interface, 1, &server,
        [](wl_client* client, void* data, uint32_t version, uint32_t id) {
            wl_resource* r = wl_resource_create(client, &wp_pointer_warp_v1_interface, int(version), id);
            if (!r) {
                wl_client_post_no_memory(client);
                return;
            }
            wl_resource_set_implementation(r, &warp_impl, data, nullptr);
        });
}

} // namespace atrium
