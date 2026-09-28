#pragma once
#include "listener.hpp"
#include "wl/resource.hpp"

#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

namespace atrium {

class Server;
namespace wl {
class ExtBackgroundEffectManagerV1;
class ExtBackgroundEffectSurfaceV1;
}

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
        wl::Weak<wl::ExtBackgroundEffectSurfaceV1> resource;
        wlr_surface* surface;
        pixman_region32_t pending, current;
        bool requested = false;  // set_blur_region has been committed at least once
        Listener<> commit, destroy;
    };

    void get_background_effect(wl::ExtBackgroundEffectManagerV1* manager, uint32_t id, wl_resource* surface);
    void surface_gone(Effect* effect);
    void effect_gone(Effect* effect);
    uint32_t capabilities() const;

    Server& server_;
    std::unique_ptr<wl::Global> global_;
    std::vector<wl::Weak<wl::ExtBackgroundEffectManagerV1>> managers_;
    std::unordered_map<wlr_surface*, Effect*> effects_;
    std::vector<Effect*> all_;  // including ones whose surface is gone
};

} // namespace atrium
