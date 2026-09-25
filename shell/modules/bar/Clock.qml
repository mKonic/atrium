import QtQuick
import Atrium.Shell
import Atrium
import shell.components
import shell.services

// The date and time; opens the month (CalendarPanel).
Pill {
    id: root

    readonly property bool open: Panels.open === "calendar"

    color: open ? Theme.palette.accentFill : Theme.material.pill
    implicitWidth: row.implicitWidth + Theme.padding.normal * 2 + Theme.padding.small

    SystemClock {
        id: clock

        precision: SystemClock.Minutes
    }

    // The time zone changed in Settings: the new time now.
    Connections {
        target: DateTime

        function onChanged(): void {
            clock.refresh();
        }
    }

    Row {
        id: row

        anchors.centerIn: parent
        spacing: Theme.spacing.small

        MaterialIcon {
            anchors.verticalCenter: parent.verticalCenter
            text: "calendar_month"
            font.pointSize: Theme.font.size.normal
            color: Theme.palette.accent
        }

        StyledText {
            anchors.verticalCenter: parent.verticalCenter
            text: Qt.formatDateTime(clock.date, (Atrium.settings["clock.24_hour"] ?? false) ? "ddd d MMM   H:mm" : "ddd d MMM   h:mm AP")
            font.weight: Font.Medium
        }
    }

    TapHandler {
        onTapped: Panels.toggle("calendar")
    }
}
