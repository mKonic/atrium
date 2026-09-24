#pragma once
// The accent colour (appearance.accent): macOS's set. Plain C++: the
// compositor, the QML plugin and the tests all use it.
//
// Tones are CIELAB lightness (0 black, 100 white) at the accent's hue, with as
// much of its chroma as sRGB holds there (GTK's accent shades).

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace atrium::accent {

// "multicolor" (macOS's default: the system blue), then macOS's colours.
const std::vector<std::string_view>& names();

// 0xRRGGBB as drawn in dark mode, or none for "multicolor" and unknown names.
std::optional<uint32_t> seed(std::string_view name);

// 0xRRGGBB as drawn in `light` or dark mode; multicolour and unknown names
// are the system blue.
uint32_t rgb(std::string_view name, bool light);

// `rgb` at lightness `tone`, its chroma capped at `max_chroma` and cut to
// what sRGB can show there.
uint32_t tone(uint32_t rgb, double tone, double max_chroma = 1000);

// CIELAB lightness of a colour: its tone.
double lightness(uint32_t rgb);

// What Settings calls it: "Multicolour", "Blue", ...
std::string_view label(std::string_view name);

// GNOME's accent-color for it (libadwaita apps), the nearest of its nine.
std::string_view gnome_name(std::string_view name);

} // namespace atrium::accent
