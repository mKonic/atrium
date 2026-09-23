#pragma once
#include "listener.hpp"

#include <optional>
#include <unordered_map>
#include <vector>

namespace atrium {

class Server;

// ext-background-effect-v1: an app asks for blur behind parts of its window
// (a translucent terminal, a sidebar). atrium honours it where it draws
// translucent windows at all, with appearance.transparency and blur on, and
// says so through the capabilities it announces.
class BackgroundEffects {
public:
    explicit BackgroundEffects(Server& server);
    ~BackgroundEffects();
    BackgroundEffects(const BackgroundEffects&) = delete;
    BackgroundEffects& operator=(const BackgroundEffects&) = delete;

    // What `surface` asked for: nothing (nullopt), no blur (an empty box), or
    // blur over this box, in surface coordinates.
    std::optional<wlr_box> blur_for(wlr_surface* surface) const;
    // The settings changed: tell the apps whether blur is on offer.
    void announce();

private:
    struct Effect {
        BackgroundEffects* owner;
        wl_resource* resource;
        wlr_surface* surface;
        pixman_region32_t pending, current;
        bool requested = false;  // set_blur_region has been committed at least once
        Listener<> commit, destroy;
    };

    static void bind(wl_client* client, void* data, uint32_t version, uint32_t id);
    static void get_background_effect(wl_client* client, wl_resource* manager, uint32_t id, wl_resource* surface);
    static void set_blur_region(wl_client* client, wl_resource* resource, wl_resource* region);
    static void destroy_resource(wl_client* client, wl_resource* resource);
    static void effect_gone(wl_resource* resource);
    static void manager_gone(wl_resource* resource);
    void surface_gone(Effect* effect);
    uint32_t capabilities() const;

    Server& server_;
    wl_global* global_ = nullptr;
    std::vector<wl_resource*> managers_;
    std::unordered_map<wlr_surface*, Effect*> effects_;
    std::vector<Effect*> all_;  // including ones whose surface is gone
};

} // namespace atrium
