#pragma once

namespace atrium {

// Install atrium's GTK theme into $XDG_DATA_HOME/themes/atrium and point apps
// started in the session at it (GTK_THEME), so the header-bar buttons of GTK
// apps match atrium's own title bars. Files are rewritten only when they differ.
void install_gtk_theme();

// Put GTK's header-bar buttons where atrium's are (minimize, maximize, close at
// the right). This is GNOME's global setting, so only a real atrium session
// touches it, never a nested one.
void apply_gtk_button_layout();

} // namespace atrium
