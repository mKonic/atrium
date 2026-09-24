#include "night_light.hpp"

#include "ipc.hpp"
#include "night_light_core.hpp"
#include "output.hpp"
#include "server.hpp"

#include <array>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace atrium {

namespace {

constexpr int kRampMs = 3000;
constexpr int kStepMs = 33;

// Where the time zone says the computer is: /etc/localtime's zone in the
// zone tables. Also brings this process's clock to a zone changed since.
std::optional<night::Coord> here() {
    std::error_code ec;
    const std::string link = std::filesystem::read_symlink("/etc/localtime", ec).string();
    const size_t at = link.find("zoneinfo/");
    if (at == std::string::npos)
        return std::nullopt;
    const std::string zone = link.substr(at + 9);
    const char* tz = std::getenv("TZ");
    if (!tz || std::string_view(tz) != ":" + zone) {
        setenv("TZ", (":" + zone).c_str(), 1);
        tzset();
    }
    for (const char* table : {"/usr/share/zoneinfo/zone1970.tab", "/usr/share/zoneinfo/zone.tab"}) {
        std::ifstream f(table);
        std::stringstream text;
        text << f.rdbuf();
        if (auto c = night::zone_coordinates(text.str(), zone))
            return c;
    }
    return std::nullopt;
}

// `minutes` after midnight UTC of the local date of `day`.
time_t on_date(const std::tm& day, double minutes) {
    std::tm utc{};
    utc.tm_year = day.tm_year;
    utc.tm_mon = day.tm_mon;
    utc.tm_mday = day.tm_mday;
    return timegm(&utc) + time_t(std::lround(minutes * 60));
}

// Today's local midnight + `minutes`, `days_ahead` days on.
time_t local_at(time_t now, int minutes, int days_ahead) {
    std::tm t{};
    localtime_r(&now, &t);
    t.tm_mday += days_ahead;
    t.tm_hour = minutes / 60;
    t.tm_min = minutes % 60;
    t.tm_sec = 0;
    t.tm_isdst = -1;
    return mktime(&t);
}

} // namespace

NightLight::NightLight(Server& server) : server_(server) {
    tick_ = wl_event_loop_add_timer(server_.loop, [](void* data) {
        static_cast<NightLight*>(data)->update();
        return 0;
    }, this);
    ramp_ = wl_event_loop_add_timer(server_.loop, [](void* data) {
        static_cast<NightLight*>(data)->step();
        return 0;
    }, this);
    update();
}

NightLight::~NightLight() {
    if (tick_)
        wl_event_source_remove(tick_);
    if (ramp_)
        wl_event_source_remove(ramp_);
    if (transform_)
        wlr_color_transform_unref(transform_);
}

bool NightLight::scheduled(time_t now, time_t& next) {
    next = 0;
    const Config& c = server_.config;
    if (c.night_light == "always")
        return true;
    if (c.night_light == "custom") {
        const int from = night::parse_hhmm(c.night_light_from).value_or(22 * 60);
        const int to = night::parse_hhmm(c.night_light_to).value_or(7 * 60);
        std::tm t{};
        localtime_r(&now, &t);
        const int minute = t.tm_hour * 60 + t.tm_min;
        const bool on = night::in_window(minute, from, to);
        // The next of the two edges still to come.
        const int edge = on ? to : from;
        next = local_at(now, edge, edge > minute ? 0 : 1);
        return on;
    }
    if (c.night_light != "sunset")
        return false;
    const auto where = here();
    if (!where)
        return false;
    std::tm today{};
    localtime_r(&now, &today);
    auto sun = [&](int days_ahead) {
        std::tm d = today;
        d.tm_mday += days_ahead;
        d.tm_isdst = -1;
        const time_t noon = mktime(&d);
        localtime_r(&noon, &d);
        const night::SunTimes s = night::sun_times(*where, d.tm_year + 1900, d.tm_mon + 1, d.tm_mday);
        return std::tuple{s, on_date(d, s.rise), on_date(d, s.set)};
    };
    const auto [s, rise, set] = sun(0);
    if (s.always_up || s.always_down)
        return s.always_down;
    if (now < rise) {
        next = rise;
        return true;
    }
    if (now < set) {
        next = set;
        return false;
    }
    next = std::get<1>(sun(1));
    return true;
}

void NightLight::update() {
    const time_t now = time(nullptr);
    if (override_ && override_until_ && now >= override_until_)
        override_.reset();
    const nlohmann::json before = state();
    const bool on = scheduled(now, next_);
    active_ = override_.value_or(on);
    // Turned on in the afternoon, it stays on through the evening's
    // schedule: it lasts until the schedule's flip after that.
    until_ = next_;
    if (override_ && override_until_) {
        time_t after = 0;
        scheduled(override_until_ + 1, after);
        until_ = after;
    } else if (override_) {
        until_ = 0;
    }
    const int target = active_ ? night::kelvin_for(server_.config.night_light_warmth) : 6500;
    if (target != target_) {
        target_ = target;
        wl_event_source_timer_update(ramp_, 1);
    }
    if (state() != before && server_.ipc)
        server_.ipc->broadcast("night_light", {{"event", "night_light.changed"}, {"night_light", state()}});
    // Again at the top of the next minute.
    wl_event_source_timer_update(tick_, int(60 - now % 60) * 1000);
}

void NightLight::set_active(bool on) {
    time_t next = 0;
    const bool by_schedule = scheduled(time(nullptr), next);
    if (on == by_schedule) {
        override_.reset();
    } else {
        override_ = on;
        override_until_ = next;
    }
    update();
}

nlohmann::json NightLight::state() const {
    // Whether any screen can show it (a nested one can't).
    bool available = false;
    for (const Output* o : server_.outputs)
        available = available || wlr_output_get_gamma_size(o->wlr) > 0;
    return {
        {"available", available},
        {"mode", server_.config.night_light},
        {"active", active_},
        {"kelvin", target_},
        {"until", until_},
    };
}

void NightLight::step() {
    const double per_step = (6500 - night::kelvin_for(100)) * double(kStepMs) / kRampMs;
    const double diff = target_ - kelvin_;
    kelvin_ = std::abs(diff) <= per_step ? target_ : kelvin_ + std::copysign(per_step, diff);
    set_transform(kelvin_);
    if (kelvin_ != target_)
        wl_event_source_timer_update(ramp_, kStepMs);
}

void NightLight::set_transform(double kelvin) {
    wlr_color_transform* next = nullptr;
    linear_white_ = {};
    if (kelvin < 6500) {
        const night::Rgb wp = night::whitepoint(int(std::lround(kelvin)));
        // The table scales encoded values; the same look in linear light.
        linear_white_ = {std::pow(wp.r, 2.2), std::pow(wp.g, 2.2), std::pow(wp.b, 2.2)};
        constexpr size_t n = 256;
        std::array<uint16_t, n> r, g, b;
        for (size_t i = 0; i < n; ++i) {
            const double v = double(i) / (n - 1) * 65535;
            r[i] = uint16_t(std::lround(v * wp.r));
            g[i] = uint16_t(std::lround(v * wp.g));
            b[i] = uint16_t(std::lround(v * wp.b));
        }
        next = wlr_color_transform_init_lut_3x1d(n, r.data(), g.data(), b.data());
    }
    if (transform_)
        wlr_color_transform_unref(transform_);
    transform_ = next;
    ++generation_;
    for (Output* o : server_.outputs)
        wlr_output_schedule_frame(o->wlr);
}

} // namespace atrium
