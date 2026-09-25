#pragma once
// atrium's colours: macOS's semantic roles, resolved for dark or light and
// appearance.accent. Callers name what a colour is for (a label, a fill, a
// separator), never a shade or an opacity, so one palette serves solid and
// glass surfaces alike. The shell draws with it and Qt apps get it as a KDE
// colour scheme. Plain C++, tested.

#include <cstdint>
#include <string>
#include <string_view>

namespace atrium::palette {

// Every colour is 0xRRGGBBAA.
struct Palette {
    // Text and symbols, most to least prominent: body text; subtitles and
    // captions; placeholders and disabled text; watermarks.
    uint32_t label, secondary_label, tertiary_label, quaternary_label;
    // Backgrounds of controls and grouped content over any surface, thickest
    // to thinnest: slider tracks and pressed rows; switches off and hovered
    // rows; cards and fields; the faintest grouping.
    uint32_t fill, secondary_fill, tertiary_fill, quaternary_fill;
    // Hairlines between content, and the border of a panel or card.
    uint32_t separator;
    // Opaque surfaces: a window or panel with transparency off; the content
    // area inside one (lists, text fields); a raised control face (buttons);
    // the thumb of a slider or switch; grouped content (Settings' cards),
    // a step lighter than the window in both appearances.
    uint32_t window_background, control_background, control, thumb, grouped_background;
    // The accent, the text and symbols drawn on it, a translucent wash of it
    // for selected rows, and the keyboard focus ring.
    uint32_t accent, on_accent, accent_fill, focus_ring;
    // macOS's system colours: status (red, orange, yellow, green) and
    // anything that needs a colour of its own.
    uint32_t red, orange, yellow, green, mint, teal, cyan, blue, indigo, purple, pink, brown, gray;
    // Drop shadows, and the dim behind a modal dialog.
    uint32_t shadow, scrim;
};

Palette make(bool light, std::string_view accent);

// "#rrggbb", or Qt's "#aarrggbb" when translucent.
std::string hex(uint32_t rgba);

// `top` composited over opaque `bottom`: an opaque 0xRRGGBBAA.
uint32_t over(uint32_t top, uint32_t bottom);

// A KDE colour scheme (.colors) for Qt apps, from the same palette.
std::string kde_colors(bool light, std::string_view accent);

} // namespace atrium::palette
