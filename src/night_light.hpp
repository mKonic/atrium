#pragma once
// Night light: warmer colours on every screen by schedule (sunset to
// sunrise where the time zone says you are, set hours, or always), through
// each output's hardware colour table (the renderer's tint on an HDR
// screen). An app that sets gamma itself
// (gammastep, wlsunset) keeps its screen. The Control Center turns it on or
// off until the schedule next changes, as macOS's Night Shift.

#include "night_light_core.hpp"
#include "wlr.hpp"

#include <nlohmann/json.hpp>

#include <ctime>
#include <optional>

namespace atrium {

class Server;

class NightLight {
public:
    explicit NightLight(Server& server);
    ~NightLight();
    NightLight(const NightLight&) = delete;
    NightLight& operator=(const NightLight&) = delete;

    // Settings changed, or a minute passed.
    void update();
    // The schedule changed: whatever the Control Center said gives way.
    void schedule_changed() {
        override_.reset();
        update();
    }
    // On or off now, until the schedule next changes.
    void set_active(bool on);
    // {available, mode, active, kelvin, until}: until is when it next
    // changes by itself (Unix seconds), 0 for never; available, whether any
    // screen can show it.
    nlohmann::json state() const;

    // For the outputs: the colour table to show, null for none, and a
    // number that changes whenever it does.
    wlr_color_transform* transform() const { return transform_; }
    uint64_t generation() const { return generation_; }
    // The same warmth as multipliers in linear light, for HDR screens where
    // the renderer applies it instead of a gamma table (1, 1, 1 when off).
    night::Rgb linear_white() const { return linear_white_; }

private:
    bool scheduled(time_t now, time_t& next);  // on by the schedule; `next`: when that flips (0 never)
    void step();                                // toward the target, a little
    void set_transform(double kelvin);

    Server& server_;
    wl_event_source* tick_ = nullptr;
    wl_event_source* ramp_ = nullptr;
    std::optional<bool> override_;
    time_t override_until_ = 0;
    bool active_ = false;
    time_t next_ = 0;   // when the schedule next flips
    time_t until_ = 0;  // when what's shown next changes: past an override, the flip after
    int target_ = 6500;
    double kelvin_ = 6500;
    wlr_color_transform* transform_ = nullptr;
    night::Rgb linear_white_;
    uint64_t generation_ = 0;
};

} // namespace atrium
