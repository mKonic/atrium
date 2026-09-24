#pragma once
// The accent colour (appearance.accent), macOS's set, and the tones the shell
// and apps draw with it. Plain C++: the compositor, the QML plugin and the
// tests all use it.
//
// Tones are CIELAB lightness (0 black, 100 white) at the accent's hue, with as
// much of its chroma as sRGB holds there: the Material 3 idea, at the levels
// the default (caelestia's) palette uses.

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace atrium::accent {

// "multicolor" (the default palette, untouched), then macOS's colours.
const std::vector<std::string_view>& names();

// 0xRRGGBB, or none for "multicolor" and unknown names.
std::optional<uint32_t> seed(std::string_view name);

// `rgb` at lightness `tone`, its chroma capped at `max_chroma` and cut to
// what sRGB can show there.
uint32_t tone(uint32_t rgb, double tone, double max_chroma = 1000);

// CIELAB lightness of a colour: its tone.
double lightness(uint32_t rgb);

struct Tones {
    uint32_t primary, on_primary, primary_container, on_primary_container;
    uint32_t secondary_container, on_secondary_container;
};
Tones tones(uint32_t rgb, bool light);

// What Settings calls it: "Multicolour", "Blue", ...
std::string_view label(std::string_view name);

// GNOME's accent-color for it (libadwaita apps), the nearest of its nine.
std::string_view gnome_name(std::string_view name);

} // namespace atrium::accent
