import QtQuick
import shell.components
import shell.services

// Opens Control Center.
Pill {
    id: root

    // For screen readers (AT-SPI).
    Accessible.role: Accessible.Button
    Accessible.name: qsTr("Control Center")
    Accessible.onPressAction: Panels.toggle("control")

    readonly property bool open: Panels.open === "control"

    implicitWidth: implicitHeight
    color: open ? Theme.bar.selected : Theme.bar.pill

    Glass {}

    MaterialIcon {
        color: Theme.bar.palette.label
        anchors.centerIn: parent
        text: "tune"
        fill: root.open ? 1 : 0
        font.pointSize: Theme.font.size.larger
    }

    TapHandler {
        onTapped: Panels.toggle("control")
    }
}
