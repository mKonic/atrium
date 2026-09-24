#pragma once
// atrium's colours: the default palette (the user's caelestia scheme) in dark
// and light, with appearance.accent's tones swapped in. The shell draws with
// it and Qt apps get it as a KDE colour scheme. Plain C++, tested.

#include <cstdint>
#include <string>
#include <string_view>

namespace atrium::palette {

// Material 3 roles, 0xRRGGBB.
struct Palette {
    uint32_t primary, on_primary, primary_container, on_primary_container;
    uint32_t secondary_container, on_secondary_container;
    uint32_t surface, surface_container, surface_container_high;
    uint32_t on_surface, on_surface_variant, outline, outline_variant;
};

Palette make(bool light, std::string_view accent);

// "#rrggbb"
std::string hex(uint32_t rgb);

// A KDE colour scheme (.colors) for Qt apps, from the same palette.
std::string kde_colors(bool light, std::string_view accent);

} // namespace atrium::palette
