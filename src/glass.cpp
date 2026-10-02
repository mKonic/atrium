#include "glass.hpp"

#include "config.hpp"
#include "layer_surface.hpp"
#include "palette.hpp"
#include "view.hpp"
#include "wlr.hpp"
#include "server.hpp"

#include "atrium-glass-v1-server.hpp"

#include <algorithm>

namespace atrium {

using wl::AtriumGlassManagerV1;
using wl::AtriumGlassV1;

GlassShapes::GlassShapes(Server& server) : server_(server) {
    global_ = wl::Global::create<AtriumGlassManagerV1>(server.display, 2,
        [this](wl_client* client, uint32_t version, uint32_t id) {
            auto* m = wl::make<AtriumGlassManagerV1>(client, version, id);
            if (!m)
                return;
            m->on_get_glass([this](AtriumGlassManagerV1* self, uint32_t id, wl_resource* surface) {
                get_glass(self, id, surface);
            });
            std::erase_if(managers_, [](const auto& w) { return !w; });
            managers_.push_back(m);
        });
}

// Clients may outlive us by a moment: what they hold turns inert.
GlassShapes::~GlassShapes() {
    global_.reset();
    for (auto& m : managers_)
        if (m)
            m->detach();
    for (Glass* g : all_) {
        if (g->resource)
            g->resource->detach();
        delete g;
    }
}

const std::vector<GlassShape>* GlassShapes::shapes_for(wl::Surface* surface) const {
    auto it = glass_.find(surface);
    return it == glass_.end() || !it->second->committed ? nullptr : &it->second->current;
}

void GlassShapes::refresh(wl::Surface* surface) {
    Owner o = Server::owner_of(surface);
    if (o.layer)
        o.layer->refresh_blur();
    else if (o.view && o.view->mapped)
        o.view->update_decorations();
}

void apply_glass(scene::Blur* blur, const std::vector<GlassShape>& given, float dx, float dy, int width,
                 int height, double lensing, const Config& c) {
    // Big panes (a sidebar, a window's page) are frosted more, as Apple's
    // "regular" glass is next to the "clear" of small controls: text on them
    // has to stay legible over whatever is behind.
    float side = 0;
    for (const GlassShape& g : given)
        side = std::max(side, std::min(g.width, g.height));
    const bool pane = side > 240;
    blur->set_strength(c.glass_tinted || pane ? 0.45f : 0.12f);
    // The bevel's width, from the panel's short side (a bar is thin glass,
    // Control Center thick), and the slab's height, which sets how far light
    // bends in it: grown in from flat as the glass materializes.
    const float bevel = std::clamp(0.3f * float(std::min(width, height)), 8.0f, 20.0f);
    blur->set_refraction(std::max(0.01f, bevel * float(lensing) * c.glass_refraction), bevel);
    const uint32_t bg = palette::make(c.light, c.accent).window_background;
    render::GlassMaterial glass{};
    glass.tint[0] = float((bg >> 24) & 0xff) / 255;
    glass.tint[1] = float((bg >> 16) & 0xff) / 255;
    glass.tint[2] = float((bg >> 8) & 0xff) / 255;
    glass.tint[3] = c.glass_tinted ? (c.light ? 0.6f : 0.55f) : pane ? (c.light ? 0.45f : 0.4f) : (c.light ? 0.2f : 0.12f);
    glass.tint[3] = std::min(1.0f, glass.tint[3] * c.glass_tint);
    glass.adapt = c.glass_tinted ? 0.2f : 0.3f;
    glass.saturation = c.glass_tinted ? 1.2f : 1.35f;
    glass.highlight = (c.light ? 0.6f : 0.5f) * c.glass_highlight;
    glass.light_dir[0] = 0.7071f;
    glass.light_dir[1] = 0.7071f;
    glass.shadow = (c.light ? 0.16f : 0.3f) * c.glass_shadow;
    blur->set_glass(glass);

    // Its exact shapes, in the glass node's coordinates: drawn from their
    // geometry, smooth at any size.
    std::vector<scene::GlassShape> shapes;
    for (const GlassShape& g : given)
        shapes.push_back({g.x + dx, g.y + dy, g.width, g.height, g.radius, g.opacity,
                          g.clip_x + dx, g.clip_y + dy, g.clip_width, g.clip_height});
    blur->set_glass_shapes(shapes.data(), int(shapes.size()));
}

void GlassShapes::get_glass(AtriumGlassManagerV1* manager, uint32_t id, wl_resource* surface_resource) {
    wl::Surface* surface = wl::Surface::from(surface_resource);
    if (!surface)
        return;
    auto* r = wl::make<AtriumGlassV1>(manager->client(), manager->version(), id);
    if (!r)
        return;
    // A second glass for a surface replaces the first.
    if (auto it = glass_.find(surface); it != glass_.end())
        surface_gone(it->second);
    auto* g = new Glass{r, surface, {}, {}, false, {}, {}};
    glass_[surface] = g;
    all_.push_back(g);
    r->on_set_shapes([g](AtriumGlassV1*, wl_array* shapes) { take_shapes(g, shapes, 6); });
    r->on_set_clipped_shapes([g](AtriumGlassV1*, wl_array* shapes) { take_shapes(g, shapes, 10); });
    r->on_gone([this, g] { glass_gone(g); });

    g->commit = surface->events.commit.connect([g] {
        g->current = g->pending;
        g->committed = true;
        refresh(g->surface);
    });
    g->destroy = surface->events.destroy.connect([this, g] { surface_gone(g); });
}

void GlassShapes::take_shapes(Glass* g, wl_array* shapes, size_t stride) {
    if (!g->surface)
        return;
    const size_t n = shapes->size / (stride * sizeof(wl_fixed_t));
    const auto* v = static_cast<const wl_fixed_t*>(shapes->data);
    auto f = [](wl_fixed_t x) { return float(wl_fixed_to_double(x)); };
    g->pending.clear();
    for (size_t i = 0; i < n; ++i) {
        const wl_fixed_t* s = v + i * stride;
        GlassShape shape{f(s[0]), f(s[1]), f(s[2]), f(s[3]), f(s[4]), f(s[5])};
        if (stride >= 10) {
            shape.clip_x = f(s[6]);
            shape.clip_y = f(s[7]);
            shape.clip_width = f(s[8]);
            shape.clip_height = f(s[9]);
        }
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

void GlassShapes::glass_gone(Glass* g) {
    wl::Surface* surface = g->surface;
    if (surface && glass_.contains(surface) && glass_[surface] == g)
        surface_gone(g);
    std::erase(all_, g);
    delete g;
    if (surface)
        refresh(surface);
}

} // namespace atrium
