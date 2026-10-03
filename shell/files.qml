//@ pragma AppId atrium-filechooser

import QtQuick
import Atrium.Shell
import shell.modules.filechooser

// Opening or saving a file when an app asks: run by the file-chooser
// portal, which hands it the request and reads back the answer.
ShellRoot {
    FileChooserWindow {
        visible: true
    }
}
