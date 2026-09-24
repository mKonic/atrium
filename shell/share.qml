//@ pragma AppId atrium-share

import QtQuick
import Atrium.Shell
import shell.modules.share

// What to share when an app asks for the screen: run by the screen-sharing
// portal, which hands it the choices and reads back the answer.
ShellRoot {
    ShareWindow {
        visible: true
    }
}
