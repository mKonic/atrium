//@ pragma AppId atrium-notresponding

import QtQuick
import Atrium.Shell
import Atrium
import shell.modules.access
import shell.services

// "“Foot” isn't responding": Force Quit or Wait. Run by the compositor when
// an app stops answering; a card over its window, as GNOME attaches one,
// not a window of its own with a title bar and a Dock icon. Nothing else is
// dimmed or held: the rest of the desktop stays usable, and the keyboard
// stays where it was until the card is clicked.
ShellRoot {
    PanelWindow {
        id: sheet

        readonly property var around: AccessPrompt.around
        readonly property bool placed: around.width !== undefined

        visible: true
        screen: Shell.screen(around.output ?? Atrium.focusedOutput?.name)
        anchors {
            top: true
            left: true
        }
        // Centred on the window, never off the screen.
        margins {
            left: sheet.placed ? Math.max(0, Math.min(sheet.screen.width - sheet.implicitWidth, sheet.around.x + (sheet.around.width - sheet.implicitWidth) / 2))
                               : (sheet.screen.width - sheet.implicitWidth) / 2
            top: sheet.placed ? Math.max(0, Math.min(sheet.screen.height - sheet.implicitHeight, sheet.around.y + (sheet.around.height - sheet.implicitHeight) / 3))
                              : (sheet.screen.height - sheet.implicitHeight) / 3
        }
        implicitWidth: card.width + 48
        implicitHeight: card.height + 48
        exclusiveZone: -1
        color: "transparent"
        WlrLayershell.layer: WlrLayer.Top
        WlrLayershell.namespace: "atrium-notresponding"
        WlrLayershell.keyboardFocus: WlrKeyboardFocus.OnDemand

        AccessCard {
            id: card

            // Room around it for its shadow.
            anchors.centerIn: parent
        }
    }
}
