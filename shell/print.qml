//@ pragma AppId atrium-print

import QtQuick
import Atrium.Shell
import shell.modules.print

// Printing for an app: run by the print portal, which hands it what the app
// asked and reads back how to print.
ShellRoot {
    PrintWindow {
        visible: true
    }
}
