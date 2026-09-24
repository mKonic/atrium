#pragma once
#include "listener.hpp"

#include <optional>
#include <string>
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
    // Being destroyed: out of the layout, and nothing is placed on it or
    // told it is on it any more (that would hook the dying wlr_output again).
    bool dying = false;

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
    // Variable refresh: "off", "games" (while a fullscreen game is in front), "on".
    std::string adaptive_sync = "games";

    Space* active = nullptr;  // the numbered space shown here
    wlr_ext_workspace_group_handle_v1* workspace_group = nullptr;

private:
    void frame();
    void render();
    void send_frame_done();
    void presented(int64_t when, int refresh);

    // Render scheduling (see frame()).
    static constexpr int64_t kMinDelayNs = 300'000;     // not worth a timer below this
    static constexpr int64_t kMinMarginNs = 1'000'000;
    static constexpr int64_t kMarginStepNs = 250'000;
    static constexpr int kOnTimeToNarrow = 600;          // frames on time before the margin narrows
    int render_timer_fd_ = -1;
    wl_event_source* render_timer_ = nullptr;
    int64_t vblank_ns_ = 0;               // the last vblank a frame was shown at
    int64_t period_ns_ = 0;               // between vblanks
    int64_t margin_ns_ = 2'000'000;       // before the vblank compositing starts
    int64_t aimed_ns_ = 0;                // the vblank the waiting frame is aimed at
    int on_time_ = 0;

    uint64_t night_generation_ = 0;  // night light's table this screen shows
    std::optional<bool> vrr_refused_;  // a switch the screen wouldn't take, not tried again

    Listener<wlr_output_event_present> present_;
    Listener<> frame_;
    Listener<wlr_output_event_request_state> request_state_;
    Listener<> destroy_;
};

} // namespace atrium
