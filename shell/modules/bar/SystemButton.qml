import QtQuick
import QtQuick.Effects
import Quickshell
import Quickshell.Widgets
import qs.components
import qs.services
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
        color: root.open ? Theme.palette.m3SecondaryContainer
             : area.containsMouse ? Theme.alpha(Theme.palette.m3OnSurface, 0.08) : "transparent"
    }

    IconImage {
        id: logo

        anchors.centerIn: parent
        implicitSize: 18
        source: Quickshell.iconPath(SystemInfo.logo, "start-here")
        visible: false
    }

    // One colour, like the bar's other glyphs.
    MultiEffect {
        anchors.fill: logo
        source: logo
        brightness: 1
        colorization: 1
        colorizationColor: Theme.palette.m3OnSurface
    }

    MouseArea {
        id: area

        anchors.fill: parent
        hoverEnabled: true
        onClicked: Panels.toggle("system")
    }
}
