#pragma once
#include "edid.hpp"
#include "handoff.hpp"
#include "hot_corners_core.hpp"
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
    // `handoff`: what the screen showed before atrium (handoff.hpp).
    Output(Server& server, wlr_output* wlr, Scanout handoff = {});
    ~Output();
    Output(const Output&) = delete;
    Output& operator=(const Output&) = delete;

    // Position layer surfaces, recompute the usable area, and move focus to an
    // exclusive-keyboard layer surface if one exists.
    void arrange_layers();

    // Fit maximized/fullscreen views to the current boxes.
    void refit_views();

    bool enabled() const { return wlr->enabled; }
    // A secret space is drawn over it (shown, or still fading away).
    bool secret_shown() const;
    // Being destroyed: out of the layout, and nothing is placed on it or
    // told it is on it any more (that would hook the dying wlr_output again).
    bool dying = false;

    Server& server;
    wlr_output* const wlr;
    wlr_scene_output* scene_output = nullptr;
    wlr_scene_rect* fullscreen_bg = nullptr;  // hides what is behind a translucent fullscreen view

    wlr_box box{};     // whole output, layout coordinates
    wlr_box usable{};  // box minus exclusive zones of panels and docks
    std::array<hot_corners::Edge, 4> corners;  // its hot corners, by hot_corners::Corner

    std::vector<LayerSurface*> layers[4];  // indexed by zwlr_layer_shell_v1_layer

    wlr_session_lock_surface_v1* lock_surface = nullptr;
    Listener<> lock_surface_commit;
    Listener<> lock_surface_destroy;

    bool lid_off = false;  // a built-in screen atrium turned off for the closed lid
    bool asleep = false;  // turned off through wlr-output-power-management
    // Variable refresh: "off", "games" (while a fullscreen game is in front), "on".
    std::string adaptive_sync = "games";

    // HDR, as Windows does it: the screen gets an HDR10 signal (BT.2020, PQ)
    // described with its own EDID luminances, everything is composited as
    // before and SDR content's white sits at sdr_brightness (0-100: 80-480
    // nits); HDR apps keep their absolute brightness.
    bool hdr = false;
    int sdr_brightness = 30;
    // 0-100: SDR content in HDR from plain sRGB (0) to the screen's own gamut
    // (100), as the screen itself stretches it outside HDR.
    int sdr_color = 100;
    std::optional<HdrCaps> hdr_caps;  // from the screen's EDID (real screens only)
    bool hdr_supported() const;
    bool hdr_active() const;
    // Signal and compositing as set; false when the screen refused HDR.
    bool apply_hdr();
    // SDR brightness 0-100 as the nits white is shown at in HDR: 80 (what
    // SDR is mastered for) up to the screen's peak, never past it. Content
    // that says what it is (Chromium, HDR video) has its reference white
    // here too.
    double sdr_white_nits() const {
        const double peak = peak_nits();
        const double top = peak > 80 ? peak : 480.0;
        return 80.0 + (top - 80.0) * sdr_brightness / 100.0;
    }
    // The screen's peak: an HDR calibration's measured one, else its EDID's
    // (0: unknown).
    double peak_nits() const {
        if (hdr_peak_)
            return *hdr_peak_;
        return hdr_caps && hdr_caps->max_nits > 0 ? hdr_caps->max_nits : 0.0;
    }

    // A colour profile (ICC) for SDR, and an HDR calibration (a profile with
    // an MHC2 tag) for HDR, as KWin applies them: paths, empty for none.
    std::string icc, icc_hdr;
    // The one for the mode the screen is in (or `hdr`'s), into the renderer;
    // false, with why, when it can't be used (then none is).
    bool apply_icc(bool for_hdr, std::string* why = nullptr);

    Space* active = nullptr;  // the numbered space shown here
    wlr_ext_workspace_group_handle_v1* workspace_group = nullptr;

    // The screen's last frame from before atrium, over everything until the
    // shell's first picture (wallpaper or greeter) is up, then faded out.
    void place_handoff();
    // A shell surface mapped here (its layer-shell namespace).
    void handoff_mapped(std::string_view name_space);

private:
    std::optional<double> hdr_peak_;  // the HDR calibration's

    void fade_handoff();
    // Zoom: the frame's part around the pointer, drawn whole into a buffer
    // of its own, which goes on screen instead.
    void magnify(wlr_output_state& state);
    wlr_swapchain* zoom_chain_ = nullptr;
    bool zoom_cursors_ = false;  // software cursors locked, so the pointer is magnified too
    double zoom_px_ = -1, zoom_py_ = -1;  // where the pointer last was here, in buffer pixels
    wlr_scene_buffer* handoff_ = nullptr;
    wl_event_source* handoff_timer_ = nullptr;
    std::string handoff_awaits_;  // the shell surface that replaces it

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
