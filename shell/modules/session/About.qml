pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Effects
import Atrium.Shell
import shell.components
import shell.services
import Atrium

// About This Computer: the system's logo and name, and what the machine is,
// read from the hardware when the shell starts.
PanelWindow {
    id: root

    property string uptime: ""

    visible: Panels.open === "about"
    onVisibleChanged: if (visible) uptime = SystemInfo.uptime()
    screen: Shell.screen(Atrium.focusedOutput?.name)
    anchors {
        top: true
        bottom: true
        left: true
        right: true
    }
    exclusiveZone: -1
    color: "transparent"
    WlrLayershell.layer: WlrLayer.Top
    WlrLayershell.namespace: "atrium-about"
    WlrLayershell.keyboardFocus: visible ? WlrKeyboardFocus.OnDemand : WlrKeyboardFocus.None

    // A click beside the card puts it away.
    MouseArea {
        anchors.fill: parent
        onClicked: Panels.open = ""
    }

    Rectangle {
        id: card

        anchors.centerIn: parent
        width: 420
        height: column.implicitHeight + 56
        radius: 26
        color: Theme.panel(Theme.palette.m3SurfaceContainer, 0.9)

        GlassRim {}

        border.width: 1
        border.color: Theme.alpha(Theme.palette.m3Outline, 0.22)
        focus: true
        Keys.onEscapePressed: Panels.open = ""

        layer.enabled: true
        layer.effect: MultiEffect {
            shadowEnabled: true
            shadowColor: Qt.rgba(0, 0, 0, 0.5)
            shadowBlur: 1
            shadowVerticalOffset: 8
        }

        MouseArea {
            anchors.fill: parent  // clicks on the card stay on it
        }

        SystemFacts {
            id: column

            anchors.horizontalCenter: parent.horizontalCenter
            y: 30
            width: parent.width - 56
            uptime: root.uptime
        }
    }
}
