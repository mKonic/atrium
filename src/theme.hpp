#pragma once

#include <string>
#include <string_view>

namespace atrium {

// What apps draw text, icons and the pointer with; empty keeps what the
// user or their other desktop already set.
struct Interface {
    std::string icon_theme;
    std::string font;       // family, at font_size
    int font_size = 11;
    std::string mono;       // monospace family, at font_size
    std::string cursor_theme;
    int cursor_size = 24;
};

// Install atrium's GTK theme into $XDG_DATA_HOME/themes/atrium and point apps
// started in the session at it (GTK_THEME), so the header-bar buttons of GTK
// apps match atrium's own title bars. Files are rewritten only when they differ.
// `accent` is an appearance.accent name; "multicolor" keeps the theme's own.
void install_gtk_theme(bool light, std::string_view accent);
// Qt apps' colours: a KDE colour scheme from atrium's palette, through the
// qtengine platform theme (QTENGINE_CONFIG, in $XDG_DATA_HOME/atrium/qt),
// keeping the user's own qtengine style, icons and fonts; without qtengine,
// a kdeglobals for plasma-integration's platform theme. Nothing when another
// platform theme (qt6ct, ...) was chosen.
void install_qt_theme(bool light, std::string_view accent, const Interface& ui);
// Defaults for other programs that follow atrium's look through their own
// settings (fcitx5's popup), in a directory first in XDG_CONFIG_DIRS: the
// user's own ~/.config files still win.
void install_app_defaults();
// Tell apps through GSettings (read by the settings portal): libadwaita,
// Firefox, Chromium and Qt follow color-scheme live.
void apply_color_scheme(bool light);
// Icons, fonts and the cursor through GSettings, which GTK apps and the
// settings portal read, live.
void apply_interface(const Interface& ui);
// GNOME's accent-color (libadwaita apps, live through the portal).
void apply_accent_color(std::string_view accent);

// Put GTK's header-bar buttons where atrium's are (minimize, maximize, close at
// the right). This is GNOME's global setting, so only a real atrium session
// touches it, never a nested one.
void apply_gtk_button_layout();

} // namespace atrium
