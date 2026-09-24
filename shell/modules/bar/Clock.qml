import QtQuick
import Atrium.Shell
import shell.components
import shell.services

Pill {
    implicitWidth: row.implicitWidth + Theme.padding.normal * 2 + Theme.padding.small

    SystemClock {
        id: clock

        precision: SystemClock.Minutes
    }

    Row {
        id: row

        anchors.centerIn: parent
        spacing: Theme.spacing.small

        MaterialIcon {
            anchors.verticalCenter: parent.verticalCenter
            text: "calendar_month"
            font.pointSize: Theme.font.size.normal
            color: Theme.palette.m3Primary
        }

        StyledText {
            anchors.verticalCenter: parent.verticalCenter
            text: Qt.formatDateTime(clock.date, "ddd d MMM   h:mm AP")
            font.weight: Font.Medium
        }
    }
}
