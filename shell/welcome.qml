//@ pragma AppId atrium-welcome

import QtQuick
import Atrium.Shell
import shell.modules.welcome

// The first login's welcome as its own app; the shell starts it once.
ShellRoot {
    WelcomeWindow {
        visible: true
    }
}
