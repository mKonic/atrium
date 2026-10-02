#include "surface_blur.hpp"

#include "wl/compositor.hpp"

#include "listener.hpp"
#include "server.hpp"

#include <unordered_set>

namespace atrium {

scene::Buffer* main_buffer(scene::Tree* tree, wl::Surface* surface) {
    scene::Buffer* found = nullptr;
    tree->for_each_buffer([&](scene::Buffer* b, int, int) {
        if (scene::SurfaceNode* s = b->surface(); s && s->surface == surface)
            found = b;
    });
    return found;
}

namespace {

struct SurfaceBlur {
    Server& server;
    scene::Tree* tree;
    wl::Surface* surface;
    scene::Blur* blur = nullptr;
    wl::Connection commit, surface_destroy;
    Listener<> tree_destroy;

    void update() {
        const Config& c = server.config;
        scene::Buffer* mask = c.blur && c.transparency && surface && surface->mapped() ? main_buffer(tree, surface) : nullptr;
        if (!mask) {
            if (blur)
                blur->set_enabled(false);
            return;
        }
        if (!blur) {
            blur = scene::Blur::create(tree, 0, 0);
            blur->set_use_cache(false);  // the window under a menu too
        }
        int tx = 0, ty = 0, mx = 0, my = 0;
        tree->coords(&tx, &ty);
        mask->coords(&mx, &my);
        blur->lower_to_bottom();
        blur->set_enabled(true);
        blur->set_position(mx - tx, my - ty);
        blur->set_size(surface->current().width, surface->current().height);
        blur->set_mask(mask);
    }
};

std::unordered_set<SurfaceBlur*> g_blurs;

} // namespace

void attach_surface_blur(Server& server, scene::Tree* tree, wl::Surface* surface) {
    auto* s = new SurfaceBlur{server, tree, surface};
    s->commit = surface->events.commit.connect([s] { s->update(); });
    s->surface_destroy = surface->events.destroy.connect([s] {
        s->commit.disconnect();
        s->surface_destroy.disconnect();
        s->surface = nullptr;
    });
    // The blur node is the tree's child and goes with it.
    s->tree_destroy.connect(&tree->events.destroy, [s](void*) {
        g_blurs.erase(s);
        delete s;
    });
    g_blurs.insert(s);
    s->update();
}

void refresh_surface_blurs() {
    for (SurfaceBlur* s : g_blurs)
        s->update();
}

} // namespace atrium
