#pragma once
// Night light's arithmetic, without wlroots: how warm, where the sun is, and
// when it's on.

#include <optional>
#include <string_view>

namespace atrium::night {

struct Rgb {
    double r = 1, g = 1, b = 1;
};

// The colour of light at `kelvin`, scaled so 6500 K (daylight) is white.
Rgb whitepoint(int kelvin);

// Warmth as Settings shows it (0 a little, 100 a lot) in kelvin.
int kelvin_for(int warmth);

struct Coord {
    double lat = 0, lon = 0;  // degrees, north and east positive
};

// "+4230+00131" or "-332751-0703940" (ISO 6709, as the zone tables write them).
std::optional<Coord> parse_iso6709(std::string_view s);

// The coordinates of time zone `zone` in a zone1970.tab / zone.tab.
std::optional<Coord> zone_coordinates(std::string_view table, std::string_view zone);

// Sunrise and sunset on a date at a place, in minutes after that day's UTC
// midnight (can fall outside 0..1440). Near the poles the sun may not rise
// or set at all.
struct SunTimes {
    double rise = 0, set = 0;
    bool always_up = false, always_down = false;
};
SunTimes sun_times(Coord c, int year, int month, int day);

// "22:00" → minutes after midnight.
std::optional<int> parse_hhmm(std::string_view s);

// Whether `now` (minutes after midnight) falls between `from` and `to`,
// across midnight when `to` comes first.
bool in_window(int now, int from, int to);

} // namespace atrium::night
