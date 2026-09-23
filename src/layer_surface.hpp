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

    Listener<> destroy_;
    Listener<> unmap_;
    Listener<> commit_;
};

} // namespace atrium
