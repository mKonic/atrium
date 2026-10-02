#pragma once
// Night light: warmer colours on every screen by schedule (sunset to
// sunrise where the time zone says you are, set hours, or always), drawn by
// the renderer (a colour matrix in linear light; on an HDR screen, the tint
// in its HDR pipeline). An app that sets gamma itself (gammastep, wlsunset)
// keeps its screen. The Control Center turns it on or
// off until the schedule next changes, as macOS's Night Shift.

#include "night_light_core.hpp"
#include "common.hpp"

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

    // The warmth as multipliers in linear light, which the renderer draws
    // the screens through (1, 1, 1 when off).
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
    night::Rgb linear_white_{1, 1, 1};
};

} // namespace atrium
