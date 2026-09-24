import QtQuick
import QtQuick.Effects
import Atrium.Shell
import shell.components
import shell.services
import Atrium

// The distribution's logo at the bar's left end, as the Apple menu is on a
// Mac: About, settings, sleep, restart, shut down, log out.
Item {
    id: root

    readonly property bool open: Panels.open === "system"

    implicitWidth: 30
    implicitHeight: 28

    Rectangle {
        anchors.fill: parent
        radius: height / 2
        color: root.open ? Theme.palette.accentFill
             : area.containsMouse ? Theme.palette.tertiaryFill : "transparent"
    }

    IconImage {
        id: logo

        anchors.centerIn: parent
        implicitSize: 18
        source: Shell.iconPath(SystemInfo.logo, "start-here")
        visible: false
    }

    // One colour, like the bar's other glyphs.
    MultiEffect {
        anchors.fill: logo
        source: logo
        brightness: 1
        colorization: 1
        colorizationColor: Theme.palette.label
    }

    MouseArea {
        id: area

        anchors.fill: parent
        hoverEnabled: true
        onClicked: Panels.toggle("system")
    }
}
