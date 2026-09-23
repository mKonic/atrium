//@ pragma AppId atrium-settings
//@ pragma UseQApplication

import QtQuick
import Quickshell
import qs.modules.settings

// System Settings as its own app (its own app id, name and icon in the bar
// and the Dock), sharing the shell's components. The shell starts it; a
// second start while it runs does nothing, and the running one switches
// page on the shell's "settings:Page" action itself.
ShellRoot {
    Settings {
        visible: true
        page: Quickshell.env("ATRIUM_SETTINGS_PAGE") || "Appearance"
        onVisibleChanged: if (!visible) Qt.quit()
    }
}
