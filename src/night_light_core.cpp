#include "night_light_core.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <numbers>

namespace atrium::night {

namespace {

// Tanner Helland's fit of blackbody colour to sRGB, 0..255 per channel.
Rgb blackbody(double kelvin) {
    const double t = std::clamp(kelvin, 1000.0, 40000.0) / 100;
    Rgb c;
    c.r = t <= 66 ? 255 : 329.698727446 * std::pow(t - 60, -0.1332047592);
    c.g = t <= 66 ? 99.4708025861 * std::log(t) - 161.1195681661 : 288.1221695283 * std::pow(t - 60, -0.0755148492);
    c.b = t >= 66 ? 255 : t <= 19 ? 0 : 138.5177312231 * std::log(t - 10) - 305.0447927307;
    c.r = std::clamp(c.r, 0.0, 255.0);
    c.g = std::clamp(c.g, 0.0, 255.0);
    c.b = std::clamp(c.b, 0.0, 255.0);
    return c;
}

// Degrees, minutes (and seconds) from ISO 6709 digits: DDMM[SS] or DDDMM[SS].
std::optional<double> angle(std::string_view s, int degree_digits) {
    const int n = int(s.size());
    if (n < 1 + degree_digits + 2 || (s[0] != '+' && s[0] != '-'))
        return std::nullopt;
    std::string_view digits = s.substr(1);
    if (digits.size() != size_t(degree_digits + 2) && digits.size() != size_t(degree_digits + 4))
        return std::nullopt;
    auto num = [](std::string_view d) {
        int v = 0;
        auto [p, ec] = std::from_chars(d.data(), d.data() + d.size(), v);
        return ec == std::errc() && p == d.data() + d.size() ? std::optional<int>(v) : std::nullopt;
    };
    auto deg = num(digits.substr(0, degree_digits));
    auto min = num(digits.substr(degree_digits, 2));
    auto sec = digits.size() > size_t(degree_digits + 2) ? num(digits.substr(degree_digits + 2, 2)) : std::optional<int>(0);
    if (!deg || !min || !sec)
        return std::nullopt;
    const double v = *deg + *min / 60.0 + *sec / 3600.0;
    return s[0] == '-' ? -v : v;
}

} // namespace

Rgb whitepoint(int kelvin) {
    if (kelvin >= 6500)
        return {};
    const Rgb c = blackbody(kelvin), white = blackbody(6500);
    return {std::min(1.0, c.r / white.r), std::min(1.0, c.g / white.g), std::min(1.0, c.b / white.b)};
}

int kelvin_for(int warmth) {
    warmth = std::clamp(warmth, 0, 100);
    return 5500 - warmth * 30;  // 5500 K to 2500 K
}

std::optional<Coord> parse_iso6709(std::string_view s) {
    // The longitude starts at the second sign.
    const size_t split = s.find_first_of("+-", 1);
    if (split == std::string_view::npos)
        return std::nullopt;
    auto lat = angle(s.substr(0, split), 2);
    auto lon = angle(s.substr(split), 3);
    if (!lat || !lon)
        return std::nullopt;
    return Coord{*lat, *lon};
}

std::optional<Coord> zone_coordinates(std::string_view table, std::string_view zone) {
    while (!table.empty()) {
        const size_t nl = table.find('\n');
        std::string_view line = table.substr(0, nl);
        table.remove_prefix(nl == std::string_view::npos ? table.size() : nl + 1);
        if (line.empty() || line[0] == '#')
            continue;
        // codes <tab> coordinates <tab> zone [<tab> comment]
        const size_t t1 = line.find('\t');
        const size_t t2 = t1 == std::string_view::npos ? t1 : line.find('\t', t1 + 1);
        if (t2 == std::string_view::npos)
            continue;
        const size_t t3 = line.find('\t', t2 + 1);
        if (line.substr(t2 + 1, t3 == std::string_view::npos ? std::string_view::npos : t3 - t2 - 1) == zone)
            return parse_iso6709(line.substr(t1 + 1, t2 - t1 - 1));
    }
    return std::nullopt;
}

SunTimes sun_times(Coord c, int year, int month, int day) {
    // NOAA's general solar position formulas.
    constexpr double pi = std::numbers::pi;
    auto leap = [](int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; };
    static constexpr int before[] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
    const int n = before[std::clamp(month, 1, 12) - 1] + day + (month > 2 && leap(year) ? 1 : 0);
    const double g = 2 * pi / (leap(year) ? 366 : 365) * (n - 1);
    const double eqtime = 229.18 * (0.000075 + 0.001868 * std::cos(g) - 0.032077 * std::sin(g) -
                                    0.014615 * std::cos(2 * g) - 0.040849 * std::sin(2 * g));
    const double decl = 0.006918 - 0.399912 * std::cos(g) + 0.070257 * std::sin(g) - 0.006758 * std::cos(2 * g) +
                        0.000907 * std::sin(2 * g) - 0.002697 * std::cos(3 * g) + 0.00148 * std::sin(3 * g);
    const double lat = c.lat * pi / 180;
    const double cos_ha = std::cos(90.833 * pi / 180) / (std::cos(lat) * std::cos(decl)) - std::tan(lat) * std::tan(decl);
    SunTimes t;
    if (cos_ha > 1) {
        t.always_down = true;
        return t;
    }
    if (cos_ha < -1) {
        t.always_up = true;
        return t;
    }
    const double ha = std::acos(cos_ha) * 180 / pi;
    t.rise = 720 - 4 * (c.lon + ha) - eqtime;
    t.set = 720 - 4 * (c.lon - ha) - eqtime;
    return t;
}

std::optional<int> parse_hhmm(std::string_view s) {
    const size_t colon = s.find(':');
    if (colon == std::string_view::npos || colon == 0 || colon > 2 || s.size() - colon != 3)
        return std::nullopt;
    int h = 0, m = 0;
    auto [p1, e1] = std::from_chars(s.data(), s.data() + colon, h);
    auto [p2, e2] = std::from_chars(s.data() + colon + 1, s.data() + s.size(), m);
    if (e1 != std::errc() || e2 != std::errc() || p1 != s.data() + colon || p2 != s.data() + s.size() || h > 23 ||
        m > 59 || h < 0 || m < 0)
        return std::nullopt;
    return h * 60 + m;
}

bool in_window(int now, int from, int to) {
    if (from == to)
        return false;
    return from < to ? now >= from && now < to : now >= from || now < to;
}

} // namespace atrium::night
