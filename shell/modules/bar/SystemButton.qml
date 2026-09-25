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

    implicitWidth: Theme.lens ? Theme.bar.inner : 30
    implicitHeight: Theme.lens ? Theme.bar.inner : 28

    // Liquid Glass: on a glass disc of its own, like the bar's pills, so it
    // reads over any wallpaper (the bar itself is clear then).
    Rectangle {
        anchors.fill: parent
        radius: height / 2
        visible: Theme.lens
        color: Theme.material.pill

        Glass {}
    }

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
        layer.enabled: true
    }

    // One colour, like the bar's other glyphs: the label colour, cut out by
    // the logo's own alpha (edges and all).
    Rectangle {
        anchors.fill: logo
        color: Theme.palette.label
        layer.enabled: true
        layer.effect: MultiEffect {
            maskEnabled: true
            maskSource: logo
            maskThresholdMin: 0.5
            maskSpreadAtMin: 1
        }
    }

    MouseArea {
        id: area

        anchors.fill: parent
        hoverEnabled: true
        onClicked: Panels.toggle("system")
    }
}
