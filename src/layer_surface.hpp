#pragma once
#include "listener.hpp"

namespace atrium {

class Output;
class Server;

// A wlr-layer-shell surface: wallpaper, panel, dock, launcher, notification.
class LayerSurface {
public:
    LayerSurface(Server& server, wlr_layer_surface_v1* wlr);
    ~LayerSurface();
    LayerSurface(const LayerSurface&) = delete;
    LayerSurface& operator=(const LayerSurface&) = delete;

    bool wants_exclusive_keyboard() const;
    // A setting that decides blur changed.
    void refresh_blur() { update_blur(); }

    Server& server;
    wlr_layer_surface_v1* const wlr;
    Output* output = nullptr;
    wlr_scene_layer_surface_v1* scene_layer = nullptr;
    wlr_scene_tree* tree = nullptr;
    wlr_scene_tree* popups = nullptr;
    bool mapped = false;

private:
    void commit();
    void unmap();
    // Frost what is behind the panel, only where it draws, per appearance.blurred_panels.
    void update_blur();

    wlr_scene_blur* blur_ = nullptr;  // in `tree`, which frees it

    Listener<> destroy_;
    Listener<> unmap_;
    Listener<> commit_;
};

} // namespace atrium
