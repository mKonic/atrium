import QtQuick
import Atrium.Shell
import Atrium
import shell.components
import shell.services

// The date and time; opens the month (CalendarPanel).
Pill {
    id: root

    // For screen readers (AT-SPI): the date and time, opening the calendar.
    Accessible.role: Accessible.Button
    Accessible.name: Qt.locale().toString(clock.date, "dddd d MMMM, " + ((Atrium.settings["clock.24_hour"] ?? false) ? "H:mm" : "h:mm AP"))
    Accessible.description: qsTr("Calendar")
    Accessible.onPressAction: Panels.toggle("calendar")

    readonly property bool open: Panels.open === "calendar"

    color: open ? Theme.bar.selected : Theme.bar.pill
    implicitWidth: row.implicitWidth + Theme.padding.normal * 2

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

        StyledText {
            color: Theme.bar.palette.label
            anchors.verticalCenter: parent.verticalCenter
            text: Qt.locale().toString(clock.date, (Atrium.settings["clock.24_hour"] ?? false) ? "ddd d MMM   H:mm" : "ddd d MMM   h:mm AP")
        }
    }

    TapHandler {
        onTapped: Panels.toggle("calendar")
    }
}
