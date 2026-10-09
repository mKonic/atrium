//@ pragma AppId atrium-notresponding

import QtQuick
import Atrium.Shell
import Atrium
import shell.modules.access
import shell.services

// "Application Not Responding": Terminate or Wait. Run by the compositor
// when an app stops answering, as Hyprland runs hyprland-dialog; a window of
// its own, so the rest of the desktop stays usable. Closing it waits.
ShellRoot {
    FloatingWindow {
        visible: true
        title: "Application Not Responding"
        color: Theme.palette.windowBackground
        implicitWidth: card.width
        implicitHeight: card.height
        onVisibleChanged: if (!visible) AccessPrompt.answer(false)

        AccessCard {
            id: card

            // The window is the frame.
            anchors.centerIn: parent
            radius: 0
            color: "transparent"
            border.width: 0
            layer.enabled: false
        }
    }
}
