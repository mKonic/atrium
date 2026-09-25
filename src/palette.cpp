#include "palette.hpp"

#include "accent.hpp"

#include <cmath>
#include <cstdio>

namespace atrium::palette {

namespace {

// macOS's values (NSColor's dynamic colours; the fills are UIKit's, which
// have the full ladder AppKit lacks).
constexpr Palette kDark{
    .label = 0xffffffd8, .secondary_label = 0xffffff8c, .tertiary_label = 0xffffff3f, .quaternary_label = 0xffffff19,
    .fill = 0x7878805c, .secondary_fill = 0x78788052, .tertiary_fill = 0x7676803d, .quaternary_fill = 0x7676802e,
    .separator = 0xffffff19,
    .window_background = 0x323232ff, .control_background = 0x1e1e1eff, .control = 0xffffff3f, .thumb = 0xdcdcdcff, .grouped_background = 0x3d3d3fff,
    .accent = 0, .on_accent = 0, .accent_fill = 0, .focus_ring = 0,
    .red = 0xff453aff, .orange = 0xff9f0aff, .yellow = 0xffd60aff, .green = 0x32d74bff, .mint = 0x63e6e2ff,
    .teal = 0x40c8e0ff, .cyan = 0x64d2ffff, .blue = 0x0a84ffff, .indigo = 0x5e5ce6ff, .purple = 0xbf5af2ff,
    .pink = 0xff375fff, .brown = 0xac8e68ff, .gray = 0x98989dff,
    .shadow = 0x00000080, .scrim = 0x00000066,
};

constexpr Palette kLight{
    .label = 0x000000d8, .secondary_label = 0x0000007f, .tertiary_label = 0x00000042, .quaternary_label = 0x00000019,
    .fill = 0x78788033, .secondary_fill = 0x78788029, .tertiary_fill = 0x7676801f, .quaternary_fill = 0x74748014,
    .separator = 0x0000001a,
    .window_background = 0xecececff, .control_background = 0xffffffff, .control = 0xffffffff, .thumb = 0xffffffff, .grouped_background = 0xffffffff,
    .accent = 0, .on_accent = 0, .accent_fill = 0, .focus_ring = 0,
    .red = 0xff3b30ff, .orange = 0xff9500ff, .yellow = 0xffcc00ff, .green = 0x28cd41ff, .mint = 0x00c7beff,
    .teal = 0x30b0c7ff, .cyan = 0x32ade6ff, .blue = 0x007affff, .indigo = 0x5856d6ff, .purple = 0xaf52deff,
    .pink = 0xff2d55ff, .brown = 0xa2845eff, .gray = 0x8e8e93ff,
    .shadow = 0x0000004d, .scrim = 0x0000004d,
};

double channel(uint32_t rgba, int shift) {
    return ((rgba >> shift) & 0xff) / 255.0;
}

// WCAG relative luminance of an opaque colour.
double luminance(uint32_t rgba) {
    auto lin = [](double c) { return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4); };
    return 0.2126 * lin(channel(rgba, 24)) + 0.7152 * lin(channel(rgba, 16)) + 0.0722 * lin(channel(rgba, 8));
}

uint32_t with_alpha(uint32_t rgba, uint32_t a) {
    return (rgba & 0xffffff00) | a;
}

} // namespace

Palette make(bool light, std::string_view accent) {
    Palette p = light ? kLight : kDark;
    p.accent = accent::rgb(accent, light) << 8 | 0xff;
    // White on the accent as macOS draws it, or black where white would not
    // read at all (yellow).
    p.on_accent = 1.05 / (luminance(p.accent) + 0.05) >= 1.6 ? 0xffffffff : 0x000000d8;
    p.accent_fill = with_alpha(p.accent, light ? 0x33 : 0x40);
    p.focus_ring = with_alpha(p.accent, 0x80);
    return p;
}

std::string hex(uint32_t rgba) {
    char out[10];
    if ((rgba & 0xff) == 0xff)
        std::snprintf(out, sizeof out, "#%06x", rgba >> 8);
    else
        std::snprintf(out, sizeof out, "#%02x%06x", rgba & 0xff, rgba >> 8);
    return out;
}

uint32_t over(uint32_t top, uint32_t bottom) {
    const double a = channel(top, 0);
    uint32_t out = 0;
    for (int shift : {24, 16, 8})
        out = out << 8 | uint32_t(std::lround((channel(top, shift) * a + channel(bottom, shift) * (1 - a)) * 255));
    return out << 8 | 0xff;
}

std::string kde_colors(bool light, std::string_view accent) {
    const Palette p = make(light, accent);
    const uint32_t visited = light ? 0x5856d6ff : 0x5e5ce6ff;  // indigo

    std::string out;
    auto set = [&](const char* key, uint32_t rgba) { out += std::string(key) + "=" + hex(rgba >> 8 << 8 | 0xff) + "\n"; };
    // One [Colors:*] group: its backgrounds and foregrounds over the shared
    // rest. KDE wants opaque colours: translucent roles are composited on bg.
    auto group = [&](const char* name, uint32_t bg, uint32_t fg, uint32_t fg_inactive) {
        out += std::string("[Colors:") + name + "]\n";
        set("BackgroundNormal", bg);
        set("BackgroundAlternate", over(p.quaternary_fill, bg));
        set("DecorationFocus", p.accent);
        set("DecorationHover", p.accent);
        set("ForegroundNormal", over(fg, bg));
        set("ForegroundInactive", over(fg_inactive, bg));
        set("ForegroundActive", p.accent);
        set("ForegroundLink", p.accent);
        set("ForegroundVisited", visited);
        set("ForegroundNegative", p.red);
        set("ForegroundNeutral", p.orange);
        set("ForegroundPositive", p.green);
        out += "\n";
    };
    const uint32_t button = over(p.control, p.window_background);
    group("Window", p.window_background, p.label, p.secondary_label);
    group("View", p.control_background, p.label, p.secondary_label);
    group("Button", button, p.label, p.secondary_label);
    group("Header", p.window_background, p.label, p.secondary_label);
    group("Tooltip", p.window_background, p.label, p.secondary_label);
    group("Complementary", p.window_background, p.label, p.secondary_label);
    group("Selection", p.accent, p.on_accent, p.on_accent);

    out += "[WM]\n";
    set("activeBackground", p.window_background);
    set("activeForeground", over(p.label, p.window_background));
    set("inactiveBackground", p.window_background);
    set("inactiveForeground", over(p.secondary_label, p.window_background));
    out += "\n[General]\nColorScheme=atrium\nName=atrium\n\n[KDE]\ncontrast=4\n";
    return out;
}

} // namespace atrium::palette
