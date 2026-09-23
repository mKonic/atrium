pragma ComponentBehavior: Bound

import QtQuick
import Quickshell
import Quickshell.Wayland
import qs.components
import qs.services

// The top bar: spaces and the focused app on the left, status on the right.
PanelWindow {
    id: bar

    anchors {
        top: true
        left: true
        right: true
    }

    implicitHeight: Theme.bar.height
    exclusiveZone: implicitHeight
    color: "transparent"
    WlrLayershell.namespace: "atrium-bar"

    Rectangle {
        anchors.fill: parent
        color: Theme.alpha(Theme.palette.m3Surface, 0.92)
    }

    Row {
        anchors.left: parent.left
        anchors.leftMargin: Theme.padding.normal
        anchors.verticalCenter: parent.verticalCenter
        spacing: Theme.spacing.normal

        Spaces {
            anchors.verticalCenter: parent.verticalCenter
            output: bar.screen?.name ?? ""
        }

        ActiveWindow {
            anchors.verticalCenter: parent.verticalCenter
        }
    }

    Row {
        anchors.right: parent.right
        anchors.rightMargin: Theme.padding.normal
        anchors.verticalCenter: parent.verticalCenter
        spacing: Theme.spacing.small

        Tray {
            anchors.verticalCenter: parent.verticalCenter
            bar: bar
        }

        Volume {
            anchors.verticalCenter: parent.verticalCenter
        }

        Clock {
            anchors.verticalCenter: parent.verticalCenter
        }
    }
}
