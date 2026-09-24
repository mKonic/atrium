pragma ComponentBehavior: Bound

import QtQuick
import Atrium.Shell
import shell.components
import shell.services
import Atrium

// The top bar: spaces and the focused app on the left, status on the right.
// On a tiled space it steps aside: everything fades but the space pill,
// which glides to the top center; the pointer at the top brings it back.
PanelWindow {
    id: bar

    anchors {
        top: true
        left: true
        right: true
    }

    implicitHeight: Theme.bar.height
    exclusiveZone: tiled ? 0 : implicitHeight
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
    readonly property bool tiled: output.tiled && !fullscreen
    // The whole bar, or just the space pill.
    readonly property bool chrome: !tiled || hover.hovered

    onFullscreenChanged: revealed = !fullscreen

    // Over a fullscreen app it waits under the app while hidden (so the app
    // keeps its whole screen, pointer and direct scanout) and comes over when
    // the pointer reaches the top edge.
    readonly property bool over: fullscreen && (revealed || content.y > -content.height)
    WlrLayershell.layer: over ? WlrLayer.Overlay : WlrLayer.Top

    Connections {
        target: output

        function onEdgeChanged() {
            if (!bar.fullscreen)
                return;
            if (output.edge === "top")
                bar.revealed = true;
            else if (!hover.hovered)
                hideTimer.restart();
        }
    }

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
        onTriggered: bar.revealed = !bar.fullscreen || hover.hovered || output.edge === "top"
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
            color: Theme.material.thin

            // Liquid Glass: no bar, each pill its own piece of glass.
            opacity: bar.chrome && !Theme.lens ? 1 : 0

            Behavior on opacity {
                Anim {}
            }

            Behavior on color {
                CAnim {}
            }
        }

        Row {
            id: leftRow

            anchors.left: parent.left
            anchors.leftMargin: Theme.padding.normal
            anchors.verticalCenter: parent.verticalCenter
            spacing: Theme.spacing.normal

            SystemButton {
                anchors.verticalCenter: parent.verticalCenter
                opacity: bar.chrome ? 1 : 0

                Behavior on opacity {
                    Anim {}
                }
            }

            // Holds the space pill's place; the pill itself floats over it.
            Item {
                id: spacesSlot

                anchors.verticalCenter: parent.verticalCenter
                width: spaces.width
                height: spaces.height
            }

            SecretSpaces {
                anchors.verticalCenter: parent.verticalCenter
                output: bar.screen?.name ?? ""
                opacity: bar.chrome ? 1 : 0

                Behavior on opacity {
                    Anim {}
                }
            }

            ActiveWindow {
                anchors.verticalCenter: parent.verticalCenter
                maxWidth: rightRow.x - leftRow.x - x - Theme.spacing.large
                opacity: bar.chrome ? 1 : 0

                Behavior on opacity {
                    Anim {}
                }
            }
        }

        Spaces {
            id: spaces

            // In its slot, or alone at the top center of a tiled space.
            x: bar.chrome ? leftRow.x + spacesSlot.x : (content.width - width) / 2
            anchors.verticalCenter: parent.verticalCenter
            output: bar.screen?.name ?? ""

            Behavior on x {
                Anim {
                    easing.bezierCurve: Theme.anim.emphasized
                }
            }
        }

        Row {
            id: rightRow

            anchors.right: parent.right
            opacity: bar.chrome ? 1 : 0

            Behavior on opacity {
                Anim {}
            }

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

            InputMenu {
                anchors.verticalCenter: parent.verticalCenter
                bar: bar
            }

            EjectMenu {
                anchors.verticalCenter: parent.verticalCenter
                bar: bar
            }

            Status {
                anchors.verticalCenter: parent.verticalCenter
            }

            BatteryIndicator {
                anchors.verticalCenter: parent.verticalCenter
            }

            UpdatesIndicator {
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
