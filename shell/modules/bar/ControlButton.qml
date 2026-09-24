import QtQuick
import qs.components
import qs.services

// Opens Control Center.
Pill {
    id: root

    readonly property bool open: Panels.open === "control"

    implicitWidth: implicitHeight
    color: open ? Theme.palette.m3SecondaryContainer : Theme.pill(Theme.palette.m3SurfaceContainer)

    MaterialIcon {
        anchors.centerIn: parent
        text: "tune"
        fill: root.open ? 1 : 0
        font.pointSize: Theme.font.size.normal
    }

    TapHandler {
        onTapped: Panels.toggle("control")
    }
}
