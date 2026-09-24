import QtQuick
import Atrium
import shell.components
import shell.services

// The battery's charge in the menu bar, on computers that have one; a
// click opens its settings.
Pill {
    id: root

    visible: Battery.present
    implicitWidth: row.implicitWidth + Theme.padding.normal * 2

    Row {
        id: row

        anchors.centerIn: parent
        spacing: Theme.spacing.small / 2

        MaterialIcon {
            anchors.verticalCenter: parent.verticalCenter
            text: Battery.glyph
            font.pointSize: Theme.font.size.normal
            color: Battery.percentage <= 10 && !Battery.plugged ? "#ff6b5f" : Theme.palette.m3OnSurface
        }

        StyledText {
            anchors.verticalCenter: parent.verticalCenter
            text: `${Battery.percentage}%`
            font.pointSize: Theme.font.size.smaller
        }
    }

    TapHandler {
        onTapped: Atrium.action("shell", "settings:Power")
    }
}
