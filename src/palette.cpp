#include "palette.hpp"

#include "accent.hpp"

#include <cmath>
#include <cstdio>

namespace atrium::palette {

namespace {

// Apple's macOS 27 UI Kit (its colour swatches, window and material layer
// styles), ~/dev/os/inspiration/apple-macos27-ui-kit.
constexpr Palette kDark{
    .label = 0xffffffff, .secondary_label = 0xffffff8c, .tertiary_label = 0xffffff40, .quaternary_label = 0xffffff1a,
    .fill = 0xffffff1a, .secondary_fill = 0xffffff14, .tertiary_fill = 0xffffff0d, .quaternary_fill = 0xffffff08,
    .separator = 0xffffff1a,
    .window_background = 0x1e1e1eff, .control_background = 0x1e1e1eff, .control = 0xffffff12, .thumb = 0xffffffff, .grouped_background = 0xffffff0d,
    .material = 0x2c2c2cff, .sidebar = 0x0f0f0fff,
    .accent = 0, .on_accent = 0, .accent_fill = 0, .focus_ring = 0,
    .red = 0xff4245ff, .orange = 0xff9230ff, .yellow = 0xffd600ff, .green = 0x30d158ff, .mint = 0x00dac3ff,
    .teal = 0x00d2e0ff, .cyan = 0x3cd3feff, .blue = 0x0091ffff, .indigo = 0x6d7cffff, .purple = 0xdb34f2ff,
    .pink = 0xff375fff, .brown = 0xb78a66ff, .gray = 0x98989dff,
    .shadow = 0x00000066, .scrim = 0x00000066,
};

constexpr Palette kLight{
    .label = 0x000000d9, .secondary_label = 0x00000080, .tertiary_label = 0x00000040, .quaternary_label = 0x0000001a,
    .fill = 0x0000001a, .secondary_fill = 0x00000014, .tertiary_fill = 0x0000000d, .quaternary_fill = 0x00000008,
    .separator = 0x0000001a,
    .window_background = 0xffffffff, .control_background = 0xffffffff, .control = 0x00000014, .thumb = 0xffffffff, .grouped_background = 0x0000000a,
    .material = 0xecececff, .sidebar = 0xf6f6f6ff,
    .accent = 0, .on_accent = 0, .accent_fill = 0, .focus_ring = 0,
    .red = 0xff383cff, .orange = 0xff8d28ff, .yellow = 0xffcc00ff, .green = 0x34c759ff, .mint = 0x00c8b3ff,
    .teal = 0x00c3d0ff, .cyan = 0x00c0e8ff, .blue = 0x0088ffff, .indigo = 0x6155f5ff, .purple = 0xcb30e0ff,
    .pink = 0xff2d55ff, .brown = 0xac7f5eff, .gray = 0x8e8e93ff,
    .shadow = 0x00000066, .scrim = 0x0000004d,
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

// macOS's Increase Contrast system colours, light and dark (Apple's Human
// Interface Guidelines, Color, as updated June 2025).
struct Contrast {
    std::string_view name;
    uint32_t light, dark;
};
constexpr Contrast kContrast[] = {
    {"red", 0xe9152dff, 0xff6165ff},    {"orange", 0xc55300ff, 0xffa056ff}, {"yellow", 0xa16a00ff, 0xfedf43ff},
    {"green", 0x008932ff, 0x4ad968ff},  {"mint", 0x008575ff, 0x54dfcbff},   {"teal", 0x008198ff, 0x3bddecff},
    {"cyan", 0x007eaeff, 0x6dd9ffff},   {"blue", 0x1e6ef4ff, 0x5cb8ffff},   {"indigo", 0x564adeff, 0xa7aaffff},
    {"purple", 0xb02fc2ff, 0xea8dffff}, {"pink", 0xe7124dff, 0xff8ac4ff},   {"brown", 0x956d51ff, 0xdba679ff},
    {"gray", 0x6c6c70ff, 0xaeaeb2ff},
};

uint32_t contrast(std::string_view name, bool light) {
    for (const Contrast& c : kContrast)
        if (c.name == name)
            return light ? c.light : c.dark;
    return 0;
}

// Apple gives no Increase Contrast values for labels and separators: text
// goes opaque and each lesser role a firm step down, hairlines and fills
// half as strong again, so every edge reads without colour.
uint32_t firmer(uint32_t rgba, uint32_t alpha) {
    return with_alpha(rgba, std::max(rgba & 0xff, alpha));
}

} // namespace

Palette make(bool light, std::string_view accent, bool high_contrast) {
    Palette p = light ? kLight : kDark;
    p.accent = accent::rgb(accent, light) << 8 | 0xff;
    if (high_contrast) {
        uint32_t* system[] = {&p.red, &p.orange, &p.yellow, &p.green, &p.mint, &p.teal, &p.cyan,
                              &p.blue, &p.indigo, &p.purple, &p.pink, &p.brown, &p.gray};
        for (size_t i = 0; i < std::size(system); ++i)
            *system[i] = light ? kContrast[i].light : kContrast[i].dark;
        // The accent as its system colour (Multicolour is blue, Graphite gray).
        const std::string_view base = accent == "graphite" ? "gray" : accent;
        if (const uint32_t c = contrast(base, light))
            p.accent = c;
        else if (!accent::seed(accent))
            p.accent = contrast("blue", light);
        p.label = with_alpha(p.label, 0xff);
        p.secondary_label = firmer(p.secondary_label, 0xc0);
        p.tertiary_label = firmer(p.tertiary_label, 0x80);
        p.quaternary_label = firmer(p.quaternary_label, 0x4c);
        p.separator = firmer(p.separator, 0x80);
        for (uint32_t* f : {&p.fill, &p.secondary_fill, &p.tertiary_fill, &p.quaternary_fill})
            *f = firmer(*f, std::min<uint32_t>(0xff, (*f & 0xff) * 3 / 2));
    }
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

std::string kde_colors(bool light, std::string_view accent, bool high_contrast) {
    const Palette p = make(light, accent, high_contrast);
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
