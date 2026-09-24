import QtQuick
import shell.components
import shell.services

// Opens Control Center.
Pill {
    id: root

    readonly property bool open: Panels.open === "control"

    implicitWidth: implicitHeight
    color: open ? Theme.palette.accentFill : Theme.material.pill

    Glass {}

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
