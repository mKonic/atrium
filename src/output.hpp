#pragma once
#include "listener.hpp"

#include <vector>

namespace atrium {

class LayerSurface;
class Server;
class Space;

class Output {
public:
    Output(Server& server, wlr_output* wlr);
    ~Output();
    Output(const Output&) = delete;
    Output& operator=(const Output&) = delete;

    // Position layer surfaces, recompute the usable area, and move focus to an
    // exclusive-keyboard layer surface if one exists.
    void arrange_layers();

    // Fit maximized/fullscreen views to the current boxes.
    void refit_views();

    bool enabled() const { return wlr->enabled; }

    Server& server;
    wlr_output* const wlr;
    wlr_scene_output* scene_output = nullptr;
    wlr_scene_rect* fullscreen_bg = nullptr;  // hides what is behind a translucent fullscreen view

    wlr_box box{};     // whole output, layout coordinates
    wlr_box usable{};  // box minus exclusive zones of panels and docks

    std::vector<LayerSurface*> layers[4];  // indexed by zwlr_layer_shell_v1_layer

    wlr_session_lock_surface_v1* lock_surface = nullptr;
    Listener<> lock_surface_commit;
    Listener<> lock_surface_destroy;

    bool asleep = false;  // turned off through wlr-output-power-management

    Space* active = nullptr;  // the numbered space shown here
    wlr_ext_workspace_group_handle_v1* workspace_group = nullptr;

private:
    void frame();

    Listener<> frame_;
    Listener<wlr_output_event_request_state> request_state_;
    Listener<> destroy_;
};

} // namespace atrium
