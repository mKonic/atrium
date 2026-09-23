#include "background_effect.hpp"

#include "server.hpp"
#include "view.hpp"

#include "ext-background-effect-v1-protocol.h"

#include <algorithm>

namespace atrium {

BackgroundEffects::BackgroundEffects(Server& server) : server_(server) {
    global_ = wl_global_create(server.display, &ext_background_effect_manager_v1_interface, 1, this, &bind);
}

BackgroundEffects::~BackgroundEffects() {
    // Clients may outlive us by a moment (the display goes after): cut every
    // resource loose so nothing calls back into freed memory.
    for (wl_resource* m : managers_) {
        wl_resource_set_user_data(m, nullptr);
        wl_resource_set_destructor(m, nullptr);
    }
    for (Effect* e : all_) {
        wl_resource_set_user_data(e->resource, nullptr);
        wl_resource_set_destructor(e->resource, nullptr);
        pixman_region32_fini(&e->pending);
        pixman_region32_fini(&e->current);
        delete e;
    }
    if (global_)
        wl_global_destroy(global_);
}

uint32_t BackgroundEffects::capabilities() const {
    return server_.config.blur && server_.config.transparency ? EXT_BACKGROUND_EFFECT_MANAGER_V1_CAPABILITY_BLUR : 0;
}

void BackgroundEffects::announce() {
    for (wl_resource* m : managers_)
        ext_background_effect_manager_v1_send_capabilities(m, capabilities());
    for (View* v : server_.views)
        v->update_decorations();
}

void BackgroundEffects::bind(wl_client* client, void* data, uint32_t version, uint32_t id) {
    auto* self = static_cast<BackgroundEffects*>(data);
    wl_resource* r = wl_resource_create(client, &ext_background_effect_manager_v1_interface, int(version), id);
    if (!r) {
        wl_client_post_no_memory(client);
        return;
    }
    static const struct ext_background_effect_manager_v1_interface impl = {
        .destroy = &destroy_resource,
        .get_background_effect = &get_background_effect,
    };
    wl_resource_set_implementation(r, &impl, self, &manager_gone);
    self->managers_.push_back(r);
    ext_background_effect_manager_v1_send_capabilities(r, self->capabilities());
}

void BackgroundEffects::destroy_resource(wl_client*, wl_resource* resource) {
    wl_resource_destroy(resource);
}

void BackgroundEffects::manager_gone(wl_resource* resource) {
    if (auto* self = static_cast<BackgroundEffects*>(wl_resource_get_user_data(resource)))
        std::erase(self->managers_, resource);
}

void BackgroundEffects::get_background_effect(wl_client* client, wl_resource* manager, uint32_t id,
                                              wl_resource* surface_resource) {
    auto* self = static_cast<BackgroundEffects*>(wl_resource_get_user_data(manager));
    wlr_surface* surface = wlr_surface_from_resource(surface_resource);
    if (!self)
        return;
    if (self->effects_.contains(surface)) {
        wl_resource_post_error(manager, EXT_BACKGROUND_EFFECT_MANAGER_V1_ERROR_BACKGROUND_EFFECT_EXISTS,
                               "this surface already has a background effect");
        return;
    }
    wl_resource* r = wl_resource_create(client, &ext_background_effect_surface_v1_interface,
                                        wl_resource_get_version(manager), id);
    if (!r) {
        wl_client_post_no_memory(client);
        return;
    }
    static const struct ext_background_effect_surface_v1_interface impl = {
        .destroy = &destroy_resource,
        .set_blur_region = &set_blur_region,
    };
    auto* e = new Effect{self, r, surface, {}, {}, false, {}, {}};
    pixman_region32_init(&e->pending);
    pixman_region32_init(&e->current);
    wl_resource_set_implementation(r, &impl, e, &effect_gone);
    self->effects_[surface] = e;
    self->all_.push_back(e);

    // Double-buffered: what was asked takes effect with the surface's commit.
    e->commit.connect(&surface->events.commit, [e](void*) {
        pixman_region32_copy(&e->current, &e->pending);
        e->requested = true;
        if (Owner o = Server::owner_of(e->surface); o.view)
            o.view->update_decorations();
    });
    e->destroy.connect(&surface->events.destroy, [e](void*) { e->owner->surface_gone(e); });
}

void BackgroundEffects::set_blur_region(wl_client*, wl_resource* resource, wl_resource* region) {
    auto* e = static_cast<Effect*>(wl_resource_get_user_data(resource));
    if (!e)
        return;
    if (!e->surface) {
        wl_resource_post_error(resource, EXT_BACKGROUND_EFFECT_SURFACE_V1_ERROR_SURFACE_DESTROYED,
                               "the surface is gone");
        return;
    }
    if (region)
        pixman_region32_copy(&e->pending, wlr_region_from_resource(region));
    else
        pixman_region32_clear(&e->pending);
}

void BackgroundEffects::surface_gone(Effect* e) {
    effects_.erase(e->surface);
    e->commit.disconnect();
    e->destroy.disconnect();
    e->surface = nullptr;
}

void BackgroundEffects::effect_gone(wl_resource* resource) {
    auto* e = static_cast<Effect*>(wl_resource_get_user_data(resource));
    if (!e)
        return;
    BackgroundEffects* self = e->owner;
    wlr_surface* surface = e->surface;
    if (surface)
        self->surface_gone(e);
    std::erase(self->all_, e);
    pixman_region32_fini(&e->pending);
    pixman_region32_fini(&e->current);
    delete e;
    // Destroying the object takes the effect away, as a null region would.
    if (surface)
        if (Owner o = Server::owner_of(surface); o.view)
            o.view->update_decorations();
}

std::optional<wlr_box> BackgroundEffects::blur_for(wlr_surface* surface) const {
    auto it = effects_.find(surface);
    if (it == effects_.end() || !it->second->requested)
        return std::nullopt;
    const pixman_box32_t* ext = pixman_region32_extents(&it->second->current);
    return wlr_box{ext->x1, ext->y1, ext->x2 - ext->x1, ext->y2 - ext->y1};
}

} // namespace atrium
