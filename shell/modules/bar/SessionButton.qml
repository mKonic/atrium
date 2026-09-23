import QtQuick
import qs.components
import qs.services

// The power button at the bar's left end: opens the session menu.
Pill {
    id: root

    readonly property bool open: Panels.open === "session"

    implicitWidth: implicitHeight
    color: open ? Theme.palette.m3SecondaryContainer : Theme.palette.m3SurfaceContainer

    MaterialIcon {
        anchors.centerIn: parent
        text: "power_settings_new"
        font.pointSize: Theme.font.size.normal
    }

    TapHandler {
        onTapped: Panels.toggle("session")
    }
}
