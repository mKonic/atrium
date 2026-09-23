pragma ComponentBehavior: Bound

import QtQuick
import Quickshell
import Quickshell.Wayland
import qs.components
import qs.services
import Atrium

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

    // Over a fullscreen app the bar hides above the screen and slides in
    // when the pointer reaches the top edge, as macOS's menu bar does.
    readonly property bool fullscreen: output.fullscreen

    OutputState {
        id: output

        name: bar.screen?.name ?? ""
    }
    property bool revealed: !fullscreen

    onFullscreenChanged: revealed = !fullscreen

    WlrLayershell.layer: fullscreen ? WlrLayer.Overlay : WlrLayer.Top

    // All of the bar's place once revealed (its content is still sliding in
    // from above then), just the top edge while hidden.
    mask: Region {
        item: bar.revealed ? place : edge
    }

    Item {
        id: place

        anchors.fill: parent
    }

    HoverHandler {
        id: hover

        onHoveredChanged: {
            if (hovered)
                bar.revealed = true;
            else if (bar.fullscreen)
                hideTimer.restart();
        }
    }

    Timer {
        id: hideTimer

        interval: 450
        onTriggered: bar.revealed = !bar.fullscreen || hover.hovered
    }

    Item {
        id: edge

        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        height: 2
    }

    Item {
        id: content

        anchors.left: parent.left
        anchors.right: parent.right
        height: parent.height
        y: bar.revealed ? 0 : -height

        Behavior on y {
            Anim {
                duration: Theme.anim.small
                easing.bezierCurve: Theme.anim.emphasizedDecel
            }
        }

        Rectangle {
            anchors.fill: parent
            color: Theme.panel(Theme.palette.m3Surface, 0.7)

            Behavior on color {
                CAnim {}
            }
        }

        Row {
            anchors.left: parent.left
            anchors.leftMargin: Theme.padding.normal
            anchors.verticalCenter: parent.verticalCenter
            spacing: Theme.spacing.normal

            SystemButton {
                anchors.verticalCenter: parent.verticalCenter
            }

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

            RecordingIndicator {
                anchors.verticalCenter: parent.verticalCenter
            }

            Tray {
                anchors.verticalCenter: parent.verticalCenter
                bar: bar
            }

            Status {
                anchors.verticalCenter: parent.verticalCenter
            }

            Volume {
                anchors.verticalCenter: parent.verticalCenter
            }

            Bell {
                anchors.verticalCenter: parent.verticalCenter
            }

            ControlButton {
                anchors.verticalCenter: parent.verticalCenter
            }

            Clock {
                anchors.verticalCenter: parent.verticalCenter
            }
        }

    }
}
