#include "background_effect.hpp"

#include "server.hpp"
#include "view.hpp"

#include "ext-background-effect-v1-server.hpp"

#include <algorithm>

namespace atrium {

using wl::ExtBackgroundEffectManagerV1;
using wl::ExtBackgroundEffectSurfaceV1;

BackgroundEffects::BackgroundEffects(Server& server) : server_(server) {
    global_ = wl::Global::create<ExtBackgroundEffectManagerV1>(server.display, 1,
        [this](wl_client* client, uint32_t version, uint32_t id) {
            auto* m = wl::make<ExtBackgroundEffectManagerV1>(client, version, id);
            if (!m)
                return;
            m->on_get_background_effect([this](ExtBackgroundEffectManagerV1* self, uint32_t id, wl_resource* surface) {
                get_background_effect(self, id, surface);
            });
            std::erase_if(managers_, [](const auto& w) { return !w; });
            managers_.push_back(m);
            m->send_capabilities(capabilities());
        });
}

// Clients may outlive us by a moment (the display goes after): what they
// hold turns inert, so nothing calls back into freed memory.
BackgroundEffects::~BackgroundEffects() {
    global_.reset();
    for (auto& m : managers_)
        if (m)
            m->detach();
    for (Effect* e : all_) {
        if (e->resource)
            e->resource->detach();
        pixman_region32_fini(&e->pending);
        pixman_region32_fini(&e->current);
        delete e;
    }
}

uint32_t BackgroundEffects::capabilities() const {
    return server_.config.blur && server_.config.transparency
               ? uint32_t(ExtBackgroundEffectManagerV1::Capability::Blur)
               : 0;
}

void BackgroundEffects::announce() {
    for (auto& m : managers_)
        if (m)
            m->send_capabilities(capabilities());
    for (View* v : server_.views)
        v->update_decorations();
}

void BackgroundEffects::get_background_effect(ExtBackgroundEffectManagerV1* manager, uint32_t id,
                                              wl_resource* surface_resource) {
    wl::Surface* surface = wl::Surface::from(surface_resource);
    if (!surface)
        return;
    if (effects_.contains(surface)) {
        manager->post_error(uint32_t(ExtBackgroundEffectManagerV1::Error::BackgroundEffectExists),
                            "this surface already has a background effect");
        return;
    }
    auto* r = wl::make<ExtBackgroundEffectSurfaceV1>(manager->client(), manager->version(), id);
    if (!r)
        return;
    auto* e = new Effect{r, surface, {}, {}, false, {}, {}};
    pixman_region32_init(&e->pending);
    pixman_region32_init(&e->current);
    effects_[surface] = e;
    all_.push_back(e);

    r->on_set_blur_region([e](ExtBackgroundEffectSurfaceV1* self, wl_resource* region) {
        if (!e->surface) {
            self->post_error(uint32_t(ExtBackgroundEffectSurfaceV1::Error::SurfaceDestroyed), "the surface is gone");
            return;
        }
        if (region)
            if (auto* r = dynamic_cast<wl::RegionResource*>(wl::WlRegion::from(region)))
                pixman_region32_copy(&e->pending, r->region.get());
        else
            pixman_region32_clear(&e->pending);
    });
    r->on_gone([this, e] { effect_gone(e); });

    // Double-buffered: what was asked takes effect with the surface's commit.
    e->commit = surface->events.commit.connect([e] {
        pixman_region32_copy(&e->current, &e->pending);
        e->requested = true;
        if (Owner o = Server::owner_of(e->surface); o.view)
            o.view->update_decorations();
    });
    e->destroy = surface->events.destroy.connect([this, e] { surface_gone(e); });
}

void BackgroundEffects::surface_gone(Effect* e) {
    effects_.erase(e->surface);
    e->commit.disconnect();
    e->destroy.disconnect();
    e->surface = nullptr;
}

void BackgroundEffects::effect_gone(Effect* e) {
    wl::Surface* surface = e->surface;
    if (surface)
        surface_gone(e);
    std::erase(all_, e);
    pixman_region32_fini(&e->pending);
    pixman_region32_fini(&e->current);
    delete e;
    // Destroying the object takes the effect away, as a null region would.
    if (surface)
        if (Owner o = Server::owner_of(surface); o.view)
            o.view->update_decorations();
}

std::optional<Box> BackgroundEffects::blur_for(wl::Surface* surface) const {
    auto it = effects_.find(surface);
    if (it == effects_.end() || !it->second->requested)
        return std::nullopt;
    const pixman_box32_t* ext = pixman_region32_extents(&it->second->current);
    return Box{ext->x1, ext->y1, ext->x2 - ext->x1, ext->y2 - ext->y1};
}

} // namespace atrium
