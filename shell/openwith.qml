//@ pragma AppId atrium-openwith

import QtQuick
import Atrium.Shell
import shell.modules.appchooser

// Which app opens a file or link, when an app asks: run by the app-chooser
// portal, which hands it the apps on offer and reads back the one picked.
ShellRoot {
    AppChooserWindow {
        visible: true
    }
}
