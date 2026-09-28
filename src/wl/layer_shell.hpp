#pragma once
#include "wl/compositor.hpp"

#include "wlr-layer-shell-unstable-v1-server.hpp"

#include <string>

namespace atrium::wl {

class Output;
class Popup;

// zwlr_layer_surface_v1: a panel, dock, wallpaper or overlay, anchored to a
// screen's edges in a layer below or above the windows.
class LayerSurface : public ZwlrLayerSurfaceV1, public Role {
public:
    static constexpr const char* kRole = "zwlr_layer_surface_v1";
    using State = SurfaceState::LayerState;

    LayerSurface(wl_client* client, uint32_t version, uint32_t id, Surface* surface, Output* output,
                 uint32_t layer, std::string name_space);
    ~LayerSurface() override;

    static LayerSurface* from(Surface* surface);

    const char* name() const override { return kRole; }
    bool precommit(Surface& surface) override;
    void commit(Surface& surface) override;

    Surface* surface() const { return surface_; }
    // The screen it asked for; null lets the compositor pick (set_output).
    Output* output() const { return output_; }
    void set_output(Output* output) { output_ = output; }
    const std::string& name_space() const { return namespace_; }
    const State& current() const;
    const State& pending() const;
    bool initialized() const { return initialized_; }
    bool configured() const { return configured_; }

    // Sends a configure at once, with the size it gets: returns its serial.
    uint32_t configure(uint32_t width, uint32_t height);
    // The compositor won't show it (its screen went): the client should go.
    void close();

    struct {
        Signal<> initial_commit;
        Signal<Popup*> new_popup;
        Signal<> destroy;
    } events;

private:
    void reset();
    void gone();

    Surface* surface_;
    Output* output_;
    std::string namespace_;
    bool initialized_ = false, configured_ = false, closed_ = false, gone_ = false;
    struct Sent {
        uint32_t serial, width, height;
    };
    std::vector<Sent> sent_;
    std::vector<Popup*> popups_;
    Connection surface_gone_;
};

// zwlr_layer_shell_v1.
class LayerShell {
public:
    explicit LayerShell(wl_display* display);
    ~LayerShell();
    LayerShell(const LayerShell&) = delete;
    LayerShell& operator=(const LayerShell&) = delete;

    Signal<LayerSurface*> new_surface;

private:
    wl_display* display_;
    std::unique_ptr<Global> global_;
    std::vector<Weak<Resource>> shells_;
};

} // namespace atrium::wl
