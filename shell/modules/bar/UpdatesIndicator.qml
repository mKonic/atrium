import QtQuick
import Atrium
import shell.components
import shell.services

// Updates waiting, in the menu bar: a click opens Software Update.
Pill {
    visible: Updates.count > 0 || Updates.installing
    implicitWidth: row.implicitWidth + Theme.padding.normal * 2

    Row {
        id: row

        anchors.centerIn: parent
        spacing: Theme.spacing.small / 2

        MaterialIcon {
            anchors.verticalCenter: parent.verticalCenter
            text: Updates.installing ? "downloading" : "system_update_alt"
            font.pointSize: Theme.font.size.normal
        }

        StyledText {
            anchors.verticalCenter: parent.verticalCenter
            text: Updates.installing ? `${Updates.progress}%` : Updates.count
            font.pointSize: Theme.font.size.smaller
        }
    }

    TapHandler {
        onTapped: Atrium.action("shell", "settings:Software Update")
    }
}
