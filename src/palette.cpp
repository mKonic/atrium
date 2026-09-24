#include "palette.hpp"

#include "accent.hpp"

#include <cstdio>

namespace atrium::palette {

namespace {

constexpr Palette kDark{
    0xbfc1ff, 0x282b60, 0x6f72ac, 0xe0e0ff, 0x44455c, 0xe1e0f9,
    0x131317, 0x1f1f23, 0x2a292e, 0xe5e1e7, 0xc7c5d1, 0x918f9a, 0x46464f,
};

constexpr Palette kLight{
    0x575a92, 0xffffff, 0xe0e0ff, 0x13154b, 0xe1e0f9, 0x181a2c,
    0xfcf8ff, 0xf0ecf4, 0xeae7ef, 0x1b1b21, 0x46464f, 0x777680, 0xc7c5d0,
};

} // namespace

Palette make(bool light, std::string_view accent) {
    Palette p = light ? kLight : kDark;
    if (const auto seed = accent::seed(accent)) {
        const accent::Tones t = accent::tones(*seed, light);
        p.primary = t.primary;
        p.on_primary = t.on_primary;
        p.primary_container = t.primary_container;
        p.on_primary_container = t.on_primary_container;
        p.secondary_container = t.secondary_container;
        p.on_secondary_container = t.on_secondary_container;
    }
    return p;
}

std::string hex(uint32_t rgb) {
    char out[8];
    std::snprintf(out, sizeof out, "#%06x", rgb & 0xffffff);
    return out;
}

std::string kde_colors(bool light, std::string_view accent) {
    const Palette p = make(light, accent);
    // Link, visited and the three states: macOS's system colours.
    const uint32_t negative = light ? 0xd70015 : 0xff453a, neutral = light ? 0xc93400 : 0xff9f0a,
                   positive = light ? 0x248a3d : 0x32d74b;
    const uint32_t visited = accent::tone(0xbf5af2, light ? 40 : 75);  // purple

    std::string out;
    auto set = [&](const char* key, uint32_t rgb) { out += std::string(key) + "=" + hex(rgb) + "\n"; };
    // One [Colors:*] group: its backgrounds and foregrounds over the shared rest.
    auto group = [&](const char* name, uint32_t bg, uint32_t bg_alt, uint32_t fg, uint32_t fg_inactive) {
        out += std::string("[Colors:") + name + "]\n";
        set("BackgroundNormal", bg);
        set("BackgroundAlternate", bg_alt);
        set("DecorationFocus", p.primary);
        set("DecorationHover", p.primary);
        set("ForegroundNormal", fg);
        set("ForegroundInactive", fg_inactive);
        set("ForegroundActive", p.primary);
        set("ForegroundLink", p.primary);
        set("ForegroundVisited", visited);
        set("ForegroundNegative", negative);
        set("ForegroundNeutral", neutral);
        set("ForegroundPositive", positive);
        out += "\n";
    };
    group("Window", p.surface_container, p.surface_container_high, p.on_surface, p.outline);
    group("View", p.surface, p.surface_container, p.on_surface, p.outline);
    group("Button", p.surface_container_high, p.outline_variant, p.on_surface, p.outline);
    group("Header", p.surface_container, p.surface_container_high, p.on_surface, p.outline);
    group("Tooltip", p.surface_container_high, p.surface_container, p.on_surface, p.on_surface_variant);
    group("Complementary", p.surface, p.surface_container, p.on_surface, p.outline);
    group("Selection", p.primary, p.primary, p.on_primary, p.on_primary);

    out += "[WM]\n";
    set("activeBackground", p.surface_container);
    set("activeForeground", p.on_surface);
    set("inactiveBackground", p.surface_container);
    set("inactiveForeground", p.on_surface_variant);
    out += "\n[General]\nColorScheme=atrium\nName=atrium\n\n[KDE]\ncontrast=4\n";
    return out;
}

} // namespace atrium::palette
