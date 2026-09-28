#include "toplevel_drag.hpp"

#include "seat.hpp"
#include "server.hpp"
#include "view.hpp"

#include "xdg-toplevel-drag-v1-server.hpp"

#include <algorithm>
#include <cmath>

namespace atrium {

using wl::XdgToplevelDragManagerV1;
using wl::XdgToplevelDragV1;

ToplevelDrags::ToplevelDrags(Server& server) : server_(server) {
    global_ = wl::Global::create<XdgToplevelDragManagerV1>(server.display, 1,
        [this](wl_client* client, uint32_t version, uint32_t id) {
            auto* m = wl::make<XdgToplevelDragManagerV1>(client, version, id);
            if (!m)
                return;
            m->on_get_xdg_toplevel_drag([this](XdgToplevelDragManagerV1* self, uint32_t id, wl_resource* source) {
                get_drag(self, id, source);
            });
            std::erase_if(managers_, [](const auto& w) { return !w; });
            managers_.push_back(m);
        });
}

ToplevelDrags::~ToplevelDrags() {
    global_.reset();
    for (auto& m : managers_)
        if (m)
            m->detach();
    for (Drag* d : drags_) {
        if (d->resource)
            d->resource->detach();
        delete d;
    }
}

void ToplevelDrags::get_drag(XdgToplevelDragManagerV1* manager, uint32_t id, wl_resource* source_resource) {
    // wlroots keeps no public way from a wl_data_source to its
    // wlr_data_source, but a client's source is a wlr_client_data_source,
    // whose first member is the wlr_data_source. Null once the client
    // destroyed it.
    auto* source = static_cast<wlr_data_source*>(wl_resource_get_user_data(source_resource));
    auto* r = wl::make<XdgToplevelDragV1>(manager->client(), manager->version(), id);
    if (!r)
        return;
    if (!source) {
        r->detach();  // inert
        return;
    }
    auto* d = new Drag{r, source};
    r->on_attach([this, d](XdgToplevelDragV1* self, wl_resource* toplevel, int32_t dx, int32_t dy) {
        attach(d, self, toplevel, dx, dy);
    });
    r->on_gone([this, d] {
        std::erase(drags_, d);
        delete d;
    });
    d->source_destroy.connect(&source->events.destroy, [this, d](void*) {
        // The drag is over (dropped or cancelled) and the source with it.
        detach(*d);
        d->source = nullptr;
        d->source_destroy.disconnect();
    });
    drags_.push_back(d);
}

void ToplevelDrags::attach(Drag* d, XdgToplevelDragV1* resource, wl_resource* toplevel_resource, int32_t dx,
                           int32_t dy) {
    wlr_xdg_toplevel* toplevel = wlr_xdg_toplevel_from_resource(toplevel_resource);
    if (d->toplevel && d->toplevel->base->surface->mapped) {
        resource->post_error(uint32_t(XdgToplevelDragV1::Error::ToplevelAttached),
                             "a mapped toplevel is already attached");
        return;
    }
    detach(*d);
    if (!toplevel)
        return;
    d->toplevel = toplevel;
    d->dx = dx;
    d->dy = dy;
    // Unmapped or gone, it drops off the drag.
    d->toplevel_unmap.connect(&toplevel->base->surface->events.unmap, [this, d](void*) { detach(*d); });
    d->toplevel_destroy.connect(&toplevel->events.destroy, [this, d](void*) { detach(*d); });
    // Already on screen (dragging a whole window): it follows from now on.
    if (View* v = view_of(*d); v && v->mapped)
        motion(server_.seat->cursor->x, server_.seat->cursor->y);
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
