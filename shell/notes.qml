//@ pragma AppId atrium-notes

import QtQuick
import Atrium.Shell
import shell.modules.notes

// Notes as its own app (its own app id and window), sharing the shell's
// components. The shell starts it; a second start while it runs does
// nothing, and the running one hears the shell's "notes:..." action itself.
ShellRoot {
    NotesWindow {
        Component.onCompleted: handle(Shell.env("ATRIUM_NOTES"))
        onVisibleChanged: if (!visible) Qt.quit()
    }
}
