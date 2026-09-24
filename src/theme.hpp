#pragma once

#include <string_view>

namespace atrium {

// Install atrium's GTK theme into $XDG_DATA_HOME/themes/atrium and point apps
// started in the session at it (GTK_THEME), so the header-bar buttons of GTK
// apps match atrium's own title bars. Files are rewritten only when they differ.
// `accent` is an appearance.accent name; "multicolor" keeps the theme's own.
void install_gtk_theme(bool light, std::string_view accent);
// Qt apps' colours: a KDE colour scheme from atrium's palette, through the
// qtengine platform theme (QTENGINE_CONFIG, in $XDG_DATA_HOME/atrium/qt),
// keeping the user's own qtengine style, icons and fonts. Nothing when
// qtengine isn't installed or another platform theme was chosen.
void install_qt_theme(bool light, std::string_view accent);
// Tell apps through GSettings (read by the settings portal): libadwaita,
// Firefox, Chromium and Qt follow color-scheme live.
void apply_color_scheme(bool light);
// GNOME's accent-color (libadwaita apps, live through the portal).
void apply_accent_color(std::string_view accent);

// Put GTK's header-bar buttons where atrium's are (minimize, maximize, close at
// the right). This is GNOME's global setting, so only a real atrium session
// touches it, never a nested one.
void apply_gtk_button_layout();

} // namespace atrium
