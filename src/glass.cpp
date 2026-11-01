#include "glass.hpp"

#include "config.hpp"
#include "layer_surface.hpp"
#include "view.hpp"
#include "wlr.hpp"
#include "server.hpp"

#include "atrium-glass-v1-protocol.h"

#include <algorithm>
#include <cmath>

namespace atrium {

GlassShapes::GlassShapes(Server& server) : server_(server) {
    global_ = wl_global_create(server.display, &atrium_glass_manager_v1_interface, 3, this, &bind);
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

namespace {

// A liquid-glass style of Apple's iOS 27 UI Kit, as atrium-os's renderd
// draws it (its design/tokens.json): the backdrop blurred by `blur` points,
// made `saturation` times as rich and `brightness` as bright, bent inward
// within `depth` of the short half side by up to `distortion` of that; then
// its fills and its shadows (sigma, the kit's blur / 2).
struct KitStyle {
    float blur, saturation, brightness, depth, distortion;
    std::vector<wlr_scene_glass_fill> fills;
    std::vector<wlr_scene_glass_shadow> shadows;
};

struct KitMaterial {
    KitStyle light, dark;
};

// By GlassMaterial: regular-medium (popovers), dock (the bar and Dock),
// regular-large (dialogs), regular-small (bar items), clear.
const KitMaterial kKit[int(GlassMaterial::Count)] = {
    // regular: liquid-glass/regular-medium
    {
       // light
        {15.0f, 1.4f, 1.0f, 0.9f, 0.3f,
         {{{0.8f, 0.8f, 0.8f, 0.7f}, WLR_SCENE_GLASS_BLEND_LIGHTEN}, {{0.749f, 0.749f, 0.749f, 0.1f}, WLR_SCENE_GLASS_BLEND_DARKEN}},
         {
             {0.0f, -4.0f, 1.0f, -4.0f, {0.1529f, 0.1529f, 0.1529f, 1.0f}, WLR_SCENE_GLASS_BLEND_PLUS_LIGHTER, true},
             {0.0f, 4.0f, 4.0f, -4.0f, {0.1529f, 0.1529f, 0.1529f, 1.0f}, WLR_SCENE_GLASS_BLEND_PLUS_LIGHTER, true},
             {0.0f, -1.0f, 0.75f, -0.5f, {0.1529f, 0.1529f, 0.1529f, 1.0f}, WLR_SCENE_GLASS_BLEND_PLUS_LIGHTER, true},
             {0.0f, 1.0f, 0.75f, -0.5f, {0.1529f, 0.1529f, 0.1529f, 1.0f}, WLR_SCENE_GLASS_BLEND_PLUS_LIGHTER, true},
             {0.0f, 2.0f, 0.125f, -1.2f, {1.0f, 1.0f, 1.0f, 1.0f}, WLR_SCENE_GLASS_BLEND_NORMAL, true},
             {0.0f, 18.0f, 23.0f, 0.0f, {0.0f, 0.0f, 0.0f, 0.25f}, WLR_SCENE_GLASS_BLEND_NORMAL, false},
             {0.0f, 0.0f, 0.15f, 0.3f, {0.8588f, 0.8588f, 0.8588f, 1.0f}, WLR_SCENE_GLASS_BLEND_PLUS_DARKER, false},
             {-1.25f, 0.0f, 0.0f, -0.75f, {0.8588f, 0.8588f, 0.8588f, 1.0f}, WLR_SCENE_GLASS_BLEND_PLUS_DARKER, false},
             {1.25f, 0.0f, 0.0f, -0.75f, {0.8588f, 0.8588f, 0.8588f, 1.0f}, WLR_SCENE_GLASS_BLEND_PLUS_DARKER, false}}},
       // dark
        {30.0f, 1.2f, 1.0f, 0.9f, 0.3f,
         {{{0.102f, 0.102f, 0.102f, 0.3f}, WLR_SCENE_GLASS_BLEND_NORMAL}, {{0.102f, 0.102f, 0.102f, 0.3f}, WLR_SCENE_GLASS_BLEND_LUMINOSITY}, {{0.102f, 0.102f, 0.102f, 1.0f}, WLR_SCENE_GLASS_BLEND_LIGHTEN}},
         {
             {0.0f, -4.0f, 4.0f, -4.0f, {0.1333f, 0.1333f, 0.1333f, 1.0f}, WLR_SCENE_GLASS_BLEND_PLUS_LIGHTER, true},
             {0.0f, 4.0f, 4.0f, -4.0f, {0.1333f, 0.1333f, 0.1333f, 1.0f}, WLR_SCENE_GLASS_BLEND_PLUS_LIGHTER, true},
             {0.0f, -1.0f, 0.75f, -0.5f, {0.25f, 0.25f, 0.25f, 1.0f}, WLR_SCENE_GLASS_BLEND_PLUS_LIGHTER, true},
             {0.0f, 1.0f, 0.75f, -0.5f, {0.251f, 0.251f, 0.251f, 1.0f}, WLR_SCENE_GLASS_BLEND_PLUS_LIGHTER, true},
             {0.0f, 2.0f, 0.1f, -1.2f, {1.0f, 1.0f, 1.0f, 0.2f}, WLR_SCENE_GLASS_BLEND_NORMAL, true},
             {0.0f, 18.0f, 24.0f, 0.0f, {0.0f, 0.0f, 0.0f, 0.45f}, WLR_SCENE_GLASS_BLEND_NORMAL, false},
             {0.0f, 0.0f, 0.15f, 0.3f, {0.651f, 0.651f, 0.651f, 1.0f}, WLR_SCENE_GLASS_BLEND_PLUS_DARKER, false},
             {-1.25f, 0.0f, 0.0f, -0.75f, {0.651f, 0.651f, 0.651f, 1.0f}, WLR_SCENE_GLASS_BLEND_PLUS_DARKER, false},
             {1.25f, 0.0f, 0.0f, -0.75f, {0.65f, 0.65f, 0.65f, 1.0f}, WLR_SCENE_GLASS_BLEND_PLUS_DARKER, false}}},
    },
    // thin: liquid-glass/dock
    {
       // light
        {3.0f, 1.0f, 1.0f, 0.5f, 0.2f,
         {{{0.6f, 0.6f, 0.6f, 0.33f}, WLR_SCENE_GLASS_BLEND_LUMINOSITY}},
         {
             {-20.0f, 0.0f, 10.0f, -30.0f, {0.851f, 0.851f, 0.851f, 1.0f}, WLR_SCENE_GLASS_BLEND_PLUS_DARKER, true},
             {20.0f, 0.0f, 10.0f, -30.0f, {0.85f, 0.85f, 0.85f, 1.0f}, WLR_SCENE_GLASS_BLEND_PLUS_DARKER, true},
             {0.0f, -4.0f, 0.25f, -4.0f, {0.4f, 0.4f, 0.4f, 1.0f}, WLR_SCENE_GLASS_BLEND_PLUS_LIGHTER, true},
             {0.0f, 4.0f, 0.25f, -4.0f, {0.4f, 0.4f, 0.4f, 1.0f}, WLR_SCENE_GLASS_BLEND_PLUS_LIGHTER, true},
             {0.0f, -1.0f, 0.0f, 0.0f, {0.2f, 0.2f, 0.2f, 1.0f}, WLR_SCENE_GLASS_BLEND_PLUS_LIGHTER, true},
             {0.0f, 1.0f, 0.0f, 0.0f, {0.2f, 0.2f, 0.2f, 1.0f}, WLR_SCENE_GLASS_BLEND_PLUS_LIGHTER, true},
             {0.0f, -35.0f, 7.5f, -35.0f, {0.1f, 0.1f, 0.1f, 1.0f}, WLR_SCENE_GLASS_BLEND_PLUS_LIGHTER, true},
             {0.0f, 35.0f, 7.5f, -35.0f, {0.2f, 0.2f, 0.2f, 1.0f}, WLR_SCENE_GLASS_BLEND_PLUS_LIGHTER, true},
             {0.0f, 0.0f, 0.0f, 0.5f, {0.85f, 0.85f, 0.85f, 1.0f}, WLR_SCENE_GLASS_BLEND_PLUS_DARKER, false},
             {-1.25f, 0.0f, 0.0f, -0.75f, {0.8f, 0.8f, 0.8f, 1.0f}, WLR_SCENE_GLASS_BLEND_PLUS_DARKER, false},
             {1.25f, 0.0f, 0.0f, -0.75f, {0.8f, 0.8f, 0.8f, 1.0f}, WLR_SCENE_GLASS_BLEND_PLUS_DARKER, false}}},
       // dark
        {3.0f, 1.0f, 1.0f, 0.5f, 0.2f,
         {{{0.2f, 0.2f, 0.2f, 0.33f}, WLR_SCENE_GLASS_BLEND_LUMINOSITY}},
         {
             {-20.0f, 0.0f, 10.0f, -30.0f, {0.851f, 0.851f, 0.851f, 1.0f}, WLR_SCENE_GLASS_BLEND_PLUS_DARKER, true},
             {20.0f, 0.0f, 10.0f, -30.0f, {0.85f, 0.85f, 0.85f, 1.0f}, WLR_SCENE_GLASS_BLEND_PLUS_DARKER, true},
             {0.0f, -4.0f, 0.25f, -4.0f, {0.4f, 0.4f, 0.4f, 1.0f}, WLR_SCENE_GLASS_BLEND_PLUS_LIGHTER, true},
             {0.0f, 4.0f, 0.25f, -4.0f, {0.4f, 0.4f, 0.4f, 1.0f}, WLR_SCENE_GLASS_BLEND_PLUS_LIGHTER, true},
             {0.0f, -1.0f, 0.0f, 0.0f, {0.0667f, 0.0667f, 0.0667f, 1.0f}, WLR_SCENE_GLASS_BLEND_PLUS_LIGHTER, true},
             {0.0f, 1.0f, 0.0f, 0.0f, {0.0667f, 0.0667f, 0.0667f, 1.0f}, WLR_SCENE_GLASS_BLEND_PLUS_LIGHTER, true},
             {0.0f, -35.0f, 7.5f, -35.0f, {0.06f, 0.06f, 0.06f, 1.0f}, WLR_SCENE_GLASS_BLEND_PLUS_LIGHTER, true},
             {0.0f, 35.0f, 7.5f, -35.0f, {0.0588f, 0.0588f, 0.0588f, 1.0f}, WLR_SCENE_GLASS_BLEND_PLUS_LIGHTER, true},
             {0.0f, 0.0f, 0.0f, 0.5f, {0.85f, 0.85f, 0.85f, 1.0f}, WLR_SCENE_GLASS_BLEND_PLUS_DARKER, false},
             {-1.25f, 0.0f, 0.0f, -0.75f, {0.8f, 0.8f, 0.8f, 1.0f}, WLR_SCENE_GLASS_BLEND_PLUS_DARKER, false},
             {1.25f, 0.0f, 0.0f, -0.75f, {0.8f, 0.8f, 0.8f, 1.0f}, WLR_SCENE_GLASS_BLEND_PLUS_DARKER, false}}},
    },
    // thick: liquid-glass/regular-large
    {
       // light
        {30.0f, 1.4f, 1.0f, 0.9f, 0.3f,
         {{{1.0f, 1.0f, 1.0f, 0.7f}, WLR_SCENE_GLASS_BLEND_LIGHTEN}, {{0.749f, 0.749f, 0.749f, 0.1f}, WLR_SCENE_GLASS_BLEND_DARKEN}},
         {
             {0.0f, -4.0f, 1.0f, -4.0f, {0.1529f, 0.1529f, 0.1529f, 1.0f}, WLR_SCENE_GLASS_BLEND_PLUS_LIGHTER, true},
             {0.0f, 4.0f, 4.0f, -4.0f, {0.1529f, 0.1529f, 0.1529f, 1.0f}, WLR_SCENE_GLASS_BLEND_PLUS_LIGHTER, true},
             {0.0f, -1.0f, 0.75f, -0.5f, {0.1529f, 0.1529f, 0.1529f, 1.0f}, WLR_SCENE_GLASS_BLEND_PLUS_LIGHTER, true},
             {0.0f, 1.0f, 0.75f, -0.5f, {0.1529f, 0.1529f, 0.1529f, 1.0f}, WLR_SCENE_GLASS_BLEND_PLUS_LIGHTER, true},
             {0.0f, 2.0f, 0.125f, -1.2f, {1.0f, 1.0f, 1.0f, 1.0f}, WLR_SCENE_GLASS_BLEND_NORMAL, true},
             {0.0f, 18.0f, 23.0f, 0.0f, {0.0f, 0.0f, 0.0f, 0.25f}, WLR_SCENE_GLASS_BLEND_NORMAL, false},
             {0.0f, 0.0f, 0.15f, 0.3f, {0.8588f, 0.8588f, 0.8588f, 1.0f}, WLR_SCENE_GLASS_BLEND_PLUS_DARKER, false},
             {-1.25f, 0.0f, 0.0f, -0.75f, {0.749f, 0.749f, 0.749f, 1.0f}, WLR_SCENE_GLASS_BLEND_PLUS_DARKER, false},
             {1.25f, 0.0f, 0.0f, -0.75f, {0.749f, 0.749f, 0.749f, 1.0f}, WLR_SCENE_GLASS_BLEND_PLUS_DARKER, false}}},
       // dark
        {30.0f, 1.2f, 1.0f, 0.9f, 0.3f,
         {{{0.102f, 0.102f, 0.102f, 0.5f}, WLR_SCENE_GLASS_BLEND_NORMAL}, {{0.102f, 0.102f, 0.102f, 0.9f}, WLR_SCENE_GLASS_BLEND_LUMINOSITY}, {{0.102f, 0.102f, 0.102f, 1.0f}, WLR_SCENE_GLASS_BLEND_LIGHTEN}},
         {
             {0.0f, -4.0f, 4.0f, -4.0f, {0.1333f, 0.1333f, 0.1333f, 1.0f}, WLR_SCENE_GLASS_BLEND_PLUS_LIGHTER, true},
             {0.0f, 4.0f, 4.0f, -4.0f, {0.1333f, 0.1333f, 0.1333f, 1.0f}, WLR_SCENE_GLASS_BLEND_PLUS_LIGHTER, true},
             {0.0f, -1.0f, 0.75f, -0.5f, {0.251f, 0.251f, 0.251f, 1.0f}, WLR_SCENE_GLASS_BLEND_PLUS_LIGHTER, true},
             {0.0f, 1.0f, 0.75f, -0.5f, {0.251f, 0.251f, 0.251f, 1.0f}, WLR_SCENE_GLASS_BLEND_PLUS_LIGHTER, true},
             {0.0f, 2.0f, 0.1f, -1.2f, {1.0f, 1.0f, 1.0f, 0.2f}, WLR_SCENE_GLASS_BLEND_NORMAL, true},
             {0.0f, 18.0f, 24.0f, 0.0f, {0.0f, 0.0f, 0.0f, 0.45f}, WLR_SCENE_GLASS_BLEND_NORMAL, false},
             {0.0f, 0.0f, 0.15f, 0.3f, {0.0f, 0.0f, 0.0f, 0.5f}, WLR_SCENE_GLASS_BLEND_NORMAL, false},
             {-1.25f, 0.0f, 0.0f, -0.75f, {0.749f, 0.749f, 0.749f, 1.0f}, WLR_SCENE_GLASS_BLEND_PLUS_DARKER, false},
             {1.25f, 0.0f, 0.0f, -0.75f, {0.749f, 0.749f, 0.749f, 1.0f}, WLR_SCENE_GLASS_BLEND_PLUS_DARKER, false}}},
    },
    // control: liquid-glass/regular-small
    {
       // light
        {6.0f, 2.0f, 1.0f, 0.8f, 0.4f,
         {{{0.9733f, 0.9733f, 0.9733f, 0.2f}, WLR_SCENE_GLASS_BLEND_LUMINOSITY}, {{0.0f, 0.0f, 0.0f, 0.25f}, WLR_SCENE_GLASS_BLEND_NORMAL}, {{1.0f, 1.0f, 1.0f, 0.25f}, WLR_SCENE_GLASS_BLEND_NORMAL}, {{0.2667f, 0.2667f, 0.2667f, 0.6f}, WLR_SCENE_GLASS_BLEND_PLUS_LIGHTER}},
         {
             {0.0f, 8.0f, 7.5f, 0.0f, {0.0f, 0.0f, 0.0f, 0.02f}, WLR_SCENE_GLASS_BLEND_NORMAL, false},
             {0.0f, 0.0f, 0.0f, 0.5f, {0.86f, 0.86f, 0.86f, 1.0f}, WLR_SCENE_GLASS_BLEND_PLUS_DARKER, false},
             {-1.25f, 0.0f, 0.0f, -0.75f, {0.8588f, 0.8588f, 0.8588f, 1.0f}, WLR_SCENE_GLASS_BLEND_PLUS_DARKER, false},
             {1.25f, 0.0f, 0.0f, -0.75f, {0.8588f, 0.8588f, 0.8588f, 1.0f}, WLR_SCENE_GLASS_BLEND_PLUS_DARKER, false},
             {-20.0f, 0.0f, 10.0f, -30.0f, {0.851f, 0.851f, 0.851f, 1.0f}, WLR_SCENE_GLASS_BLEND_PLUS_DARKER, true},
             {20.0f, 0.0f, 10.0f, -30.0f, {0.85f, 0.85f, 0.85f, 1.0f}, WLR_SCENE_GLASS_BLEND_PLUS_DARKER, true},
             {0.0f, -4.0f, 0.25f, -4.0f, {0.4f, 0.4f, 0.4f, 1.0f}, WLR_SCENE_GLASS_BLEND_PLUS_LIGHTER, true},
             {0.0f, 4.0f, 0.25f, -4.0f, {0.4f, 0.4f, 0.4f, 1.0f}, WLR_SCENE_GLASS_BLEND_PLUS_LIGHTER, true},
             {0.0f, -1.0f, 0.0f, 0.0f, {0.09f, 0.09f, 0.09f, 1.0f}, WLR_SCENE_GLASS_BLEND_PLUS_LIGHTER, true},
             {0.0f, 1.0f, 0.0f, 0.0f, {0.0902f, 0.0902f, 0.0902f, 1.0f}, WLR_SCENE_GLASS_BLEND_PLUS_LIGHTER, true},
             {0.0f, 40.0f, 15.0f, -40.0f, {0.9f, 0.9f, 0.9f, 1.0f}, WLR_SCENE_GLASS_BLEND_PLUS_DARKER, true},
             {0.0f, -40.0f, 5.0f, -40.0f, {0.1569f, 0.1569f, 0.1569f, 1.0f}, WLR_SCENE_GLASS_BLEND_PLUS_LIGHTER, true},
             {0.0f, 40.0f, 5.0f, -40.0f, {0.1567f, 0.1567f, 0.1567f, 1.0f}, WLR_SCENE_GLASS_BLEND_PLUS_LIGHTER, true}}},
       // dark
        {6.0f, 1.0f, 1.0f, 0.8f, 0.4f,
         {{{0.0f, 0.0f, 0.0f, 1.0f}, WLR_SCENE_GLASS_BLEND_SCREEN}, {{0.6f, 0.6f, 0.6f, 0.17f}, WLR_SCENE_GLASS_BLEND_NORMAL}},
         {
             {20.0f, 0.0f, 10.0f, -30.0f, {0.698f, 0.698f, 0.698f, 1.0f}, WLR_SCENE_GLASS_BLEND_PLUS_DARKER, true},
             {-20.0f, 0.0f, 10.0f, -30.0f, {0.699f, 0.699f, 0.699f, 1.0f}, WLR_SCENE_GLASS_BLEND_PLUS_DARKER, true},
             {0.0f, -4.0f, 0.25f, -4.0f, {0.4f, 0.4f, 0.4f, 1.0f}, WLR_SCENE_GLASS_BLEND_PLUS_LIGHTER, true},
             {0.0f, 4.0f, 0.25f, -4.0f, {0.4f, 0.4f, 0.4f, 1.0f}, WLR_SCENE_GLASS_BLEND_PLUS_LIGHTER, true},
             {0.0f, -1.0f, 0.0f, 0.0f, {0.09f, 0.09f, 0.09f, 1.0f}, WLR_SCENE_GLASS_BLEND_PLUS_LIGHTER, true},
             {0.0f, 1.0f, 0.0f, 0.0f, {0.0902f, 0.0902f, 0.0902f, 1.0f}, WLR_SCENE_GLASS_BLEND_PLUS_LIGHTER, true},
             {0.0f, -40.0f, 5.0f, -40.0f, {0.102f, 0.102f, 0.102f, 1.0f}, WLR_SCENE_GLASS_BLEND_PLUS_LIGHTER, true},
             {0.0f, 40.0f, 5.0f, -40.0f, {0.1f, 0.1f, 0.1f, 1.0f}, WLR_SCENE_GLASS_BLEND_PLUS_LIGHTER, true},
             {0.0f, 8.0f, 7.5f, 0.0f, {0.0f, 0.0f, 0.0f, 0.04f}, WLR_SCENE_GLASS_BLEND_NORMAL, false},
             {0.0f, 0.0f, 0.0f, 0.5f, {0.749f, 0.749f, 0.749f, 1.0f}, WLR_SCENE_GLASS_BLEND_PLUS_DARKER, false},
             {-1.25f, 0.0f, 0.0f, -0.75f, {0.749f, 0.749f, 0.749f, 1.0f}, WLR_SCENE_GLASS_BLEND_PLUS_DARKER, false},
             {1.25f, 0.0f, 0.0f, -0.75f, {0.749f, 0.749f, 0.749f, 1.0f}, WLR_SCENE_GLASS_BLEND_PLUS_DARKER, false}}},
    },
    // clear: liquid-glass/clear
    {
       // light
        {3.0f, 1.0f, 1.0f, 0.5f, 0.2f,
         {{{0.9733f, 0.9733f, 0.9733f, 0.12f}, WLR_SCENE_GLASS_BLEND_LUMINOSITY}},
         {
             {0.0f, 8.0f, 7.5f, 0.0f, {0.0f, 0.0f, 0.0f, 0.02f}, WLR_SCENE_GLASS_BLEND_NORMAL, false},
             {0.0f, 0.0f, 0.0f, 0.5f, {0.85f, 0.85f, 0.85f, 1.0f}, WLR_SCENE_GLASS_BLEND_PLUS_DARKER, false},
             {-1.25f, 0.0f, 0.0f, -0.75f, {0.8f, 0.8f, 0.8f, 1.0f}, WLR_SCENE_GLASS_BLEND_PLUS_DARKER, false},
             {1.25f, 0.0f, 0.0f, -0.75f, {0.8f, 0.8f, 0.8f, 1.0f}, WLR_SCENE_GLASS_BLEND_PLUS_DARKER, false},
             {-20.0f, 0.0f, 10.0f, -30.0f, {0.851f, 0.851f, 0.851f, 1.0f}, WLR_SCENE_GLASS_BLEND_PLUS_DARKER, true},
             {20.0f, 0.0f, 10.0f, -30.0f, {0.85f, 0.85f, 0.85f, 1.0f}, WLR_SCENE_GLASS_BLEND_PLUS_DARKER, true},
             {0.0f, -4.0f, 0.25f, -4.0f, {0.4f, 0.4f, 0.4f, 1.0f}, WLR_SCENE_GLASS_BLEND_PLUS_LIGHTER, true},
             {0.0f, 4.0f, 0.25f, -4.0f, {0.4f, 0.4f, 0.4f, 1.0f}, WLR_SCENE_GLASS_BLEND_PLUS_LIGHTER, true},
             {0.0f, 40.0f, 15.0f, -40.0f, {0.9f, 0.9f, 0.9f, 1.0f}, WLR_SCENE_GLASS_BLEND_PLUS_DARKER, true},
             {0.0f, -1.0f, 0.0f, 0.0f, {0.2f, 0.2f, 0.2f, 1.0f}, WLR_SCENE_GLASS_BLEND_PLUS_LIGHTER, true},
             {0.0f, 1.0f, 0.0f, 0.0f, {0.2f, 0.2f, 0.2f, 1.0f}, WLR_SCENE_GLASS_BLEND_PLUS_LIGHTER, true},
             {0.0f, -40.0f, 10.0f, -40.0f, {0.251f, 0.251f, 0.251f, 1.0f}, WLR_SCENE_GLASS_BLEND_PLUS_LIGHTER, true},
             {0.0f, 40.0f, 10.0f, -40.0f, {0.25f, 0.25f, 0.25f, 1.0f}, WLR_SCENE_GLASS_BLEND_PLUS_LIGHTER, true}}},
       // dark
        {3.0f, 1.0f, 1.0f, 0.5f, 0.2f,
         {{{0.9733f, 0.9733f, 0.9733f, 0.12f}, WLR_SCENE_GLASS_BLEND_LUMINOSITY}},
         {
             {0.0f, 8.0f, 7.5f, 0.0f, {0.0f, 0.0f, 0.0f, 0.02f}, WLR_SCENE_GLASS_BLEND_NORMAL, false},
             {0.0f, 0.0f, 0.0f, 0.5f, {0.85f, 0.85f, 0.85f, 1.0f}, WLR_SCENE_GLASS_BLEND_PLUS_DARKER, false},
             {-1.25f, 0.0f, 0.0f, -0.75f, {0.8f, 0.8f, 0.8f, 1.0f}, WLR_SCENE_GLASS_BLEND_PLUS_DARKER, false},
             {1.25f, 0.0f, 0.0f, -0.75f, {0.8f, 0.8f, 0.8f, 1.0f}, WLR_SCENE_GLASS_BLEND_PLUS_DARKER, false},
             {-20.0f, 0.0f, 10.0f, -30.0f, {0.851f, 0.851f, 0.851f, 1.0f}, WLR_SCENE_GLASS_BLEND_PLUS_DARKER, true},
             {20.0f, 0.0f, 10.0f, -30.0f, {0.85f, 0.85f, 0.85f, 1.0f}, WLR_SCENE_GLASS_BLEND_PLUS_DARKER, true},
             {0.0f, -4.0f, 0.25f, -4.0f, {0.4f, 0.4f, 0.4f, 1.0f}, WLR_SCENE_GLASS_BLEND_PLUS_LIGHTER, true},
             {0.0f, 4.0f, 0.25f, -4.0f, {0.4f, 0.4f, 0.4f, 1.0f}, WLR_SCENE_GLASS_BLEND_PLUS_LIGHTER, true},
             {0.0f, 40.0f, 15.0f, -40.0f, {0.9f, 0.9f, 0.9f, 1.0f}, WLR_SCENE_GLASS_BLEND_PLUS_DARKER, true},
             {0.0f, -1.0f, 0.0f, 0.0f, {0.2f, 0.2f, 0.2f, 1.0f}, WLR_SCENE_GLASS_BLEND_PLUS_LIGHTER, true},
             {0.0f, 1.0f, 0.0f, 0.0f, {0.2f, 0.2f, 0.2f, 1.0f}, WLR_SCENE_GLASS_BLEND_PLUS_LIGHTER, true},
             {0.0f, -40.0f, 10.0f, -40.0f, {0.251f, 0.251f, 0.251f, 1.0f}, WLR_SCENE_GLASS_BLEND_PLUS_LIGHTER, true},
             {0.0f, 40.0f, 10.0f, -40.0f, {0.25f, 0.25f, 0.25f, 1.0f}, WLR_SCENE_GLASS_BLEND_PLUS_LIGHTER, true}}},
    },
};

const KitStyle& style_of(GlassMaterial m, const Config& c) {
    const KitMaterial& k = kKit[std::clamp(int(m), 0, int(GlassMaterial::Count) - 1)];
    return c.light ? k.light : k.dark;
}

} // namespace

int glass_reach(const std::vector<GlassShape>& shapes, const Config& c) {
    float reach = 0;
    for (const GlassShape& g : shapes)
        for (const wlr_scene_glass_shadow& sh : style_of(g.material, c).shadows)
            if (!sh.inner)
                reach = std::max(reach, std::abs(sh.x) + std::abs(sh.y) + sh.spread + 3 * sh.sigma + 1);
    return int(std::ceil(reach * std::max(c.glass_shadow, 1.0f)));
}

void apply_glass(wlr_scene_blur* blur, const std::vector<GlassShape>& given, float dx, float dy, double lensing,
                 const Config& c) {
    // The settings scale the kit's look: its fills (tint; Tinted thickens
    // them), the light along its edges inside (highlight), its drop shadow
    // and how far it bends what is behind.
    const float tint = c.glass_tint * (c.glass_tinted ? 1.5f : 1.0f);
    wlr_scene_glass glass{};
    glass.material_count = int(GlassMaterial::Count);
    for (int m = 0; m < glass.material_count; ++m) {
        const KitStyle& k = style_of(GlassMaterial(m), c);
        wlr_scene_glass_material& out = glass.materials[m];
        out.saturation = k.saturation;
        out.brightness = k.brightness;
        out.depth = k.depth;
        out.distortion = k.distortion * float(lensing) * c.glass_refraction;
        out.fill_count = std::min<int>(int(k.fills.size()), WLR_SCENE_GLASS_MAX_FILLS);
        for (int i = 0; i < out.fill_count; ++i) {
            out.fills[i] = k.fills[i];
            out.fills[i].color[3] = std::min(1.0f, out.fills[i].color[3] * tint);
        }
        out.shadow_count = std::min<int>(int(k.shadows.size()), WLR_SCENE_GLASS_MAX_SHADOWS);
        for (int i = 0; i < out.shadow_count; ++i) {
            wlr_scene_glass_shadow& sh = out.shadows[i] = k.shadows[i];
            const float scale = sh.inner ? c.glass_highlight
                                : sh.blend == WLR_SCENE_GLASS_BLEND_NORMAL ? c.glass_shadow
                                                                           : 1.0f;
            sh.color[3] = std::min(1.0f, sh.color[3] * scale);
        }
    }
    wlr_scene_blur_set_glass(blur, &glass);

    // Frosted as much as its frostiest shape: renderd blurs a style's
    // `blur` points with a dual Kawase of log2(blur) levels at offset 1,
    // which is the node's blur at the strength that sizes it so.
    float frost = 1;
    for (const GlassShape& g : given)
        frost = std::max(frost, style_of(g.material, c).blur);
    const int levels = std::clamp(int(std::lround(std::log2(frost))), 1, 6);
    const float full = std::exp2(float(c.blur_passes + 1)) * float(std::max(c.blur_radius, 1));
    wlr_scene_blur_set_strength(blur, std::min(1.0f, std::exp2(float(levels + 1)) / full));

    // Its exact shapes, in the glass node's coordinates.
    std::vector<wlr_scene_glass_shape> shapes;
    for (const GlassShape& g : given)
        shapes.push_back({g.x + dx, g.y + dy, g.width, g.height, g.radius, g.opacity, g.clip_x + dx, g.clip_y + dy,
                          g.clip_width, g.clip_height, int(g.material)});
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
        .set_clipped_shapes = &set_clipped_shapes,
        .set_material_shapes = &set_material_shapes,
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
    take_shapes(resource, shapes, 6);
}

void GlassShapes::set_clipped_shapes(wl_client*, wl_resource* resource, wl_array* shapes) {
    take_shapes(resource, shapes, 10);
}

void GlassShapes::set_material_shapes(wl_client*, wl_resource* resource, wl_array* shapes) {
    take_shapes(resource, shapes, 11);
}

void GlassShapes::take_shapes(wl_resource* resource, wl_array* shapes, size_t stride) {
    auto* g = static_cast<Glass*>(wl_resource_get_user_data(resource));
    if (!g || !g->surface)
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
        if (stride >= 11) {
            const int m = wl_fixed_to_int(s[10]);
            shape.material = m >= 0 && m < int(GlassMaterial::Count) ? GlassMaterial(m) : GlassMaterial::Regular;
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
