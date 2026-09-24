#include "toplevel_drag.hpp"

#include "seat.hpp"
#include "server.hpp"
#include "view.hpp"

#include "xdg-toplevel-drag-v1-protocol.h"

#include <algorithm>
#include <cmath>

namespace atrium {

ToplevelDrags::ToplevelDrags(Server& server) : server_(server) {
    global_ = wl_global_create(server.display, &xdg_toplevel_drag_manager_v1_interface, 1, this, &bind);
}

ToplevelDrags::~ToplevelDrags() {
    for (wl_resource* m : managers_) {
        wl_resource_set_user_data(m, nullptr);
        wl_resource_set_destructor(m, nullptr);
    }
    for (Drag* d : drags_) {
        wl_resource_set_user_data(d->resource, nullptr);
        wl_resource_set_destructor(d->resource, nullptr);
        delete d;
    }
    if (global_)
        wl_global_destroy(global_);
}

void ToplevelDrags::bind(wl_client* client, void* data, uint32_t version, uint32_t id) {
    auto* self = static_cast<ToplevelDrags*>(data);
    wl_resource* r = wl_resource_create(client, &xdg_toplevel_drag_manager_v1_interface, int(version), id);
    if (!r) {
        wl_client_post_no_memory(client);
        return;
    }
    static const struct xdg_toplevel_drag_manager_v1_interface impl = {
        .destroy = &destroy_resource,
        .get_xdg_toplevel_drag = &get_drag,
    };
    wl_resource_set_implementation(r, &impl, self, &manager_gone);
    self->managers_.push_back(r);
}

void ToplevelDrags::destroy_resource(wl_client*, wl_resource* resource) {
    wl_resource_destroy(resource);
}

void ToplevelDrags::manager_gone(wl_resource* resource) {
    if (auto* self = static_cast<ToplevelDrags*>(wl_resource_get_user_data(resource)))
        std::erase(self->managers_, resource);
}

void ToplevelDrags::get_drag(wl_client* client, wl_resource* manager, uint32_t id, wl_resource* source_resource) {
    auto* self = static_cast<ToplevelDrags*>(wl_resource_get_user_data(manager));
    // wlroots keeps no public way from a wl_data_source to its
    // wlr_data_source, but a client's source is a wlr_client_data_source,
    // whose first member is the wlr_data_source. Null once the client
    // destroyed it.
    auto* source = static_cast<wlr_data_source*>(wl_resource_get_user_data(source_resource));
    wl_resource* r = wl_resource_create(client, &xdg_toplevel_drag_v1_interface, wl_resource_get_version(manager), id);
    if (!r) {
        wl_client_post_no_memory(client);
        return;
    }
    static const struct xdg_toplevel_drag_v1_interface impl = {
        .destroy = &destroy_resource,
        .attach = &attach,
    };
    if (!self || !source) {
        wl_resource_set_implementation(r, &impl, nullptr, nullptr);  // inert
        return;
    }
    auto* d = new Drag{self, r, source};
    wl_resource_set_implementation(r, &impl, d, &drag_gone);
    d->source_destroy.connect(&source->events.destroy, [d](void*) {
        // The drag is over (dropped or cancelled) and the source with it.
        d->owner->detach(*d);
        d->source = nullptr;
        d->source_destroy.disconnect();
    });
    self->drags_.push_back(d);
}

void ToplevelDrags::drag_gone(wl_resource* resource) {
    auto* d = static_cast<Drag*>(wl_resource_get_user_data(resource));
    if (!d)
        return;
    std::erase(d->owner->drags_, d);
    delete d;
}

void ToplevelDrags::attach(wl_client*, wl_resource* resource, wl_resource* toplevel_resource, int32_t dx, int32_t dy) {
    auto* d = static_cast<Drag*>(wl_resource_get_user_data(resource));
    if (!d)
        return;
    wlr_xdg_toplevel* toplevel = wlr_xdg_toplevel_from_resource(toplevel_resource);
    if (d->toplevel && d->toplevel->base->surface->mapped) {
        wl_resource_post_error(resource, XDG_TOPLEVEL_DRAG_V1_ERROR_TOPLEVEL_ATTACHED,
                               "a mapped toplevel is already attached");
        return;
    }
    d->owner->detach(*d);
    if (!toplevel)
        return;
    d->toplevel = toplevel;
    d->dx = dx;
    d->dy = dy;
    // Unmapped or gone, it drops off the drag.
    d->toplevel_unmap.connect(&toplevel->base->surface->events.unmap, [d](void*) { d->owner->detach(*d); });
    d->toplevel_destroy.connect(&toplevel->events.destroy, [d](void*) { d->owner->detach(*d); });
    // Already on screen (dragging a whole window): it follows from now on.
    if (View* v = d->owner->view_of(*d); v && v->mapped)
        d->owner->motion(d->owner->server_.seat->cursor->x, d->owner->server_.seat->cursor->y);
}

void ToplevelDrags::detach(Drag& d) {
    d.toplevel = nullptr;
    d.toplevel_unmap.disconnect();
    d.toplevel_destroy.disconnect();
}

ToplevelDrags::Drag* ToplevelDrags::current() const {
    const wlr_drag* drag = server_.seat->wlr->drag;
    if (!drag || !drag->source)
        return nullptr;
    for (Drag* d : drags_)
        if (d->source == drag->source && d->toplevel)
            return d;
    return nullptr;
}

View* ToplevelDrags::view_of(const Drag& d) const {
    if (!d.toplevel)
        return nullptr;
    for (View* v : server_.views)
        if (v->kind == View::Kind::Xdg && static_cast<XdgView*>(v)->toplevel == d.toplevel)
            return v;
    return nullptr;
}

View* ToplevelDrags::dragged() const {
    const Drag* d = current();
    View* v = d ? view_of(*d) : nullptr;
    return v && v->mapped ? v : nullptr;
}

void ToplevelDrags::motion(double lx, double ly) {
    const Drag* d = current();
    View* v = d ? view_of(*d) : nullptr;
    if (!v || !v->mapped || v->fullscreen)
        return;
    if (v->maximized)
        v->set_maximized(false);
    // The offset is into the window's own geometry, under atrium's title bar.
    v->move_to(int(std::lround(lx)) - d->dx, int(std::lround(ly)) - d->dy - v->top());
}

bool ToplevelDrags::place(View* view) {
    const Drag* d = current();
    if (!d || view_of(*d) != view)
        return false;
    const double lx = server_.seat->cursor->x, ly = server_.seat->cursor->y;
    view->move_to(int(std::lround(lx)) - d->dx, int(std::lround(ly)) - d->dy - view->top());
    return true;
}

} // namespace atrium
