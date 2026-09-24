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
    // Some of it is on screen, not covered: the scene sends it frame callbacks.
    bool shown_on_output() const;

private:
    void commit();
    void unmap();
    // Frost what is behind the panel, only where it draws, per appearance.blurred_panels.
    void update_blur();

    wlr_scene_blur* blur_ = nullptr;  // in `tree`, which frees it
    // Liquid Glass materializes: its lensing grows in on map (0..1).
    double lensing_ = 1;
    uint32_t keyboard_interactive_ = 0;  // as of the last commit

    Listener<> destroy_;
    Listener<> unmap_;
    Listener<> commit_;
};

} // namespace atrium
