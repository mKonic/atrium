#include "battery_core.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace atrium::battery {

bool plugged(State s) {
    return s == State::Charging || s == State::Full || s == State::PendingCharge;
}

std::string glyph(double percent, State s) {
    const double p = std::clamp(percent, 0.0, 100.0);
    if (plugged(s)) {
        if (s == State::Full || p >= 95)
            return "battery_charging_full";
        static const int steps[] = {20, 30, 50, 60, 80, 90};
        int step = 20;
        for (int v : steps)
            if (p >= v)
                step = v;
        return "battery_charging_" + std::to_string(step);
    }
    if (p <= 10)
        return "battery_alert";
    if (p >= 95)
        return "battery_full";
    // Seven bars for the rest: 0 at 10%, 6 near full.
    const int bars = int(std::lround((p - 10) / 85 * 6));
    return "battery_" + std::to_string(std::clamp(bars, 0, 6)) + "_bar";
}

std::string remaining(State s, int64_t to_empty_s, int64_t to_full_s) {
    auto clock = [](int64_t secs) {
        char buf[32];
        std::snprintf(buf, sizeof buf, "%lld:%02lld", (long long)(secs / 3600), (long long)(secs / 60 % 60));
        return std::string(buf);
    };
    switch (s) {
    case State::Full: return "Fully charged";
    case State::PendingCharge: return "Not charging";
    case State::Charging: return to_full_s > 0 ? clock(to_full_s) + " until full" : "Charging";
    case State::Discharging: return to_empty_s > 0 ? clock(to_empty_s) + " left" : "";
    default: return "";
    }
}

int warning(double percent, State s, int warned) {
    if (s != State::Discharging)
        return 0;
    const int due = percent <= 5 ? 2 : percent <= 10 ? 1 : 0;
    return due > warned ? due : 0;
}

} // namespace atrium::battery
