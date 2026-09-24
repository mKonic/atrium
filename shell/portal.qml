//@ pragma AppId atrium-portal

import QtQuick
import Atrium.Shell
import Atrium

// atrium's xdg-desktop-portal backend, started by D-Bus when the portal
// first needs it. Nothing to show until a portal asks for a picker.
ShellRoot {
    Component.onCompleted: if (!PortalBackend.start()) Qt.quit()
}
