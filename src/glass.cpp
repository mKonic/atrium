#include "glass.hpp"

#include "config.hpp"
#include "layer_surface.hpp"
#include "palette.hpp"
#include "view.hpp"
#include "wlr.hpp"
#include "server.hpp"

#include "atrium-glass-v1-protocol.h"

#include <algorithm>

namespace atrium {

GlassShapes::GlassShapes(Server& server) : server_(server) {
    global_ = wl_global_create(server.display, &atrium_glass_manager_v1_interface, 1, this, &bind);
}

GlassShapes::~GlassShapes() {
    // Clients may outlive us by a moment: cut every resource loose.
    for (wl_resource* m : managers_) {
        wl_resource_set_user_data(m, nullptr);
        wl_resource_set_destructor(m, nullptr);
    }
    for (Glass* g : all_) {
        wl_resource_set_user_data(g->resource, nullptr);
        wl_resource_set_destructor(g->resource, nullptr);
        delete g;
    }
    if (global_)
        wl_global_destroy(global_);
}

const std::vector<GlassShape>* GlassShapes::shapes_for(wlr_surface* surface) const {
    auto it = glass_.find(surface);
    return it == glass_.end() || !it->second->committed ? nullptr : &it->second->current;
}

void GlassShapes::refresh(wlr_surface* surface) {
    Owner o = Server::owner_of(surface);
    if (o.layer)
        o.layer->refresh_blur();
    else if (o.view && o.view->mapped)
        o.view->update_decorations();
}

void apply_glass(wlr_scene_blur* blur, const std::vector<GlassShape>& given, float dx, float dy, int width,
                 int height, double lensing, const Config& c) {
    // Big panes (a sidebar, a window's page) are frosted more, as Apple's
    // "regular" glass is next to the "clear" of small controls: text on them
    // has to stay legible over whatever is behind.
    float side = 0;
    for (const GlassShape& g : given)
        side = std::max(side, std::min(g.width, g.height));
    const bool pane = side > 240;
    wlr_scene_blur_set_strength(blur, c.glass_tinted || pane ? 0.45f : 0.12f);
    // The bevel's width, from the panel's short side (a bar is thin glass,
    // Control Center thick), and the slab's height, which sets how far light
    // bends in it: grown in from flat as the glass materializes.
    const float bevel = std::clamp(0.3f * float(std::min(width, height)), 8.0f, 20.0f);
    wlr_scene_blur_set_refraction(blur, std::max(0.01f, bevel * float(lensing)), bevel);
    const uint32_t bg = palette::make(c.light, c.accent).window_background;
    wlr_scene_glass glass{};
    glass.tint[0] = float((bg >> 24) & 0xff) / 255;
    glass.tint[1] = float((bg >> 16) & 0xff) / 255;
    glass.tint[2] = float((bg >> 8) & 0xff) / 255;
    glass.tint[3] = c.glass_tinted ? (c.light ? 0.6f : 0.55f) : pane ? (c.light ? 0.45f : 0.4f) : (c.light ? 0.2f : 0.12f);
    glass.adapt = c.glass_tinted ? 0.2f : 0.3f;
    glass.saturation = c.glass_tinted ? 1.2f : 1.35f;
    glass.highlight = c.light ? 0.6f : 0.5f;
    glass.light_dir[0] = 0.7071f;
    glass.light_dir[1] = 0.7071f;
    glass.shadow = c.light ? 0.16f : 0.3f;
    wlr_scene_blur_set_glass(blur, &glass);

    // Its exact shapes, in the glass node's coordinates: drawn from their
    // geometry, smooth at any size.
    std::vector<wlr_scene_glass_shape> shapes;
    for (const GlassShape& g : given)
        shapes.push_back({g.x + dx, g.y + dy, g.width, g.height, g.radius, g.opacity});
    wlr_scene_blur_set_glass_shapes(blur, shapes.data(), int(shapes.size()));
}

void GlassShapes::bind(wl_client* client, void* data, uint32_t version, uint32_t id) {
    auto* self = static_cast<GlassShapes*>(data);
    wl_resource* r = wl_resource_create(client, &atrium_glass_manager_v1_interface, int(version), id);
    if (!r) {
        wl_client_post_no_memory(client);
        return;
    }
    static const struct atrium_glass_manager_v1_interface impl = {
        .destroy = &destroy_resource,
        .get_glass = &get_glass,
    };
    wl_resource_set_implementation(r, &impl, self, &manager_gone);
    self->managers_.push_back(r);
}

void GlassShapes::destroy_resource(wl_client*, wl_resource* resource) {
    wl_resource_destroy(resource);
}

void GlassShapes::manager_gone(wl_resource* resource) {
    if (auto* self = static_cast<GlassShapes*>(wl_resource_get_user_data(resource)))
        std::erase(self->managers_, resource);
}

void GlassShapes::get_glass(wl_client* client, wl_resource* manager, uint32_t id, wl_resource* surface_resource) {
    auto* self = static_cast<GlassShapes*>(wl_resource_get_user_data(manager));
    wlr_surface* surface = wlr_surface_from_resource(surface_resource);
    if (!self)
        return;
    wl_resource* r = wl_resource_create(client, &atrium_glass_v1_interface, wl_resource_get_version(manager), id);
    if (!r) {
        wl_client_post_no_memory(client);
        return;
    }
    static const struct atrium_glass_v1_interface impl = {
        .destroy = &destroy_resource,
        .set_shapes = &set_shapes,
    };
    // A second glass for a surface replaces the first.
    if (auto it = self->glass_.find(surface); it != self->glass_.end())
        self->surface_gone(it->second);
    auto* g = new Glass{self, r, surface, {}, {}, false, {}, {}};
    wl_resource_set_implementation(r, &impl, g, &glass_gone);
    self->glass_[surface] = g;
    self->all_.push_back(g);

    g->commit.connect(&surface->events.commit, [g](void*) {
        g->current = g->pending;
        g->committed = true;
        refresh(g->surface);
    });
    g->destroy.connect(&surface->events.destroy, [g](void*) { g->owner->surface_gone(g); });
}

void GlassShapes::set_shapes(wl_client*, wl_resource* resource, wl_array* shapes) {
    auto* g = static_cast<Glass*>(wl_resource_get_user_data(resource));
    if (!g || !g->surface)
        return;
    const size_t n = shapes->size / (6 * sizeof(wl_fixed_t));
    const auto* v = static_cast<const wl_fixed_t*>(shapes->data);
    g->pending.clear();
    for (size_t i = 0; i < n; ++i) {
        const wl_fixed_t* s = v + i * 6;
        GlassShape shape{float(wl_fixed_to_double(s[0])), float(wl_fixed_to_double(s[1])),
                         float(wl_fixed_to_double(s[2])), float(wl_fixed_to_double(s[3])),
                         float(wl_fixed_to_double(s[4])), float(wl_fixed_to_double(s[5]))};
        if (shape.width > 0 && shape.height > 0 && shape.opacity > 0)
            g->pending.push_back(shape);
    }
}

void GlassShapes::surface_gone(Glass* g) {
    glass_.erase(g->surface);
    g->commit.disconnect();
    g->destroy.disconnect();
    g->surface = nullptr;
}

void GlassShapes::glass_gone(wl_resource* resource) {
    auto* g = static_cast<Glass*>(wl_resource_get_user_data(resource));
    if (!g)
        return;
    GlassShapes* self = g->owner;
    wlr_surface* surface = g->surface;
    if (surface && self->glass_.contains(surface) && self->glass_[surface] == g)
        self->surface_gone(g);
    std::erase(self->all_, g);
    delete g;
    if (surface)
        refresh(surface);
}

} // namespace atrium
