#pragma once
#include "listener.hpp"
#include "scene/scene.hpp"
#include "wl/layer_shell.hpp"

namespace atrium {

class Output;
class Server;

// A wlr-layer-shell surface: wallpaper, panel, dock, launcher, notification.
class LayerSurface {
public:
    LayerSurface(Server& server, wl::LayerSurface* ls);
    ~LayerSurface();
    LayerSurface(const LayerSurface&) = delete;
    LayerSurface& operator=(const LayerSurface&) = delete;

    bool wants_exclusive_keyboard() const;
    // A setting that decides blur changed.
    void refresh_blur() { update_blur(); }

    Server& server;
    wl::LayerSurface* const ls;
    wl::Surface* surface() const { return ls->surface(); }
    Output* output = nullptr;
    scene::LayerSurfaceNode* scene_layer = nullptr;
    scene::Tree* tree = nullptr;
    scene::Tree* popups = nullptr;
    bool mapped = false;
    // Some of it is on screen, not covered: the scene sends it frame callbacks.
    bool shown_on_output() const;

private:
    void commit();
    void unmap();
    // Frost what is behind the panel, only where it draws, per appearance.blurred_panels.
    void update_blur();

    scene::Blur* blur_ = nullptr;  // in `tree`, which frees it
    // Liquid Glass materializes: its lensing grows in on map (0..1).
    double lensing_ = 1;
    uint32_t keyboard_interactive_ = 0;  // as of the last commit

    bool initial_ = false;  // this commit was the first
    wl::Connection destroy_, unmap_, commit_, initial_commit_;
};

} // namespace atrium
