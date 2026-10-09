pragma ComponentBehavior: Bound

import QtQuick
import Atrium.Shell
import Atrium
import shell.components
import shell.services

// Identify in Displays: each screen's number, name and mode in its middle
// for 2.5 s, as KWin's Output Locator; asked again, it stays on longer.
Scope {
    id: root

    property bool shown: false

    Connections {
        target: Atrium

        function onShellAction(name: string): void {
            if (name === "identify-displays") {
                root.shown = true;
                hideTimer.restart();
            }
        }
    }

    Timer {
        id: hideTimer

        interval: 2500
        onTriggered: root.shown = false
    }

    Variants {
        model: Shell.screens

        PanelWindow {
            id: win

            required property ShellScreen modelData
            // Read afresh each time it's shown (a call isn't a binding on the outputs).
            readonly property var label: root.shown ? Atrium.identify(modelData.name) : ({})
            // KWin's badge colours, round and round.
            readonly property var badges: [Theme.palette.accent, Theme.palette.purple, Theme.palette.orange,
                                           Theme.palette.green, Theme.palette.red, Theme.palette.blue]

            screen: modelData
            visible: root.shown
            implicitWidth: card.width
            implicitHeight: card.height
            exclusiveZone: 0
            color: "transparent"
            mask: Region {}
            WlrLayershell.layer: WlrLayer.Overlay
            WlrLayershell.namespace: "atrium-identify"

            Rectangle {
                id: card

                width: row.implicitWidth
                height: row.implicitHeight
                radius: 22
                color: Theme.material.regular
                border.width: Theme.lens ? 0 : 1
                border.color: Theme.palette.separator
                clip: true

                Glass {}

                Row {
                    id: row

                    Rectangle {
                        id: badge

                        width: text.implicitHeight
                        height: text.implicitHeight
                        radius: card.radius  // the card's left corners; the right half squares them off
                        color: win.badges[((win.label.number ?? 1) - 1) % win.badges.length]

                        Rectangle {
                            anchors.right: parent.right
                            width: parent.width / 2
                            height: parent.height
                            color: parent.color
                        }

                        StyledText {
                            anchors.centerIn: parent
                            text: win.label.number ?? ""
                            font.pointSize: 40
                            font.weight: Font.Bold
                            color: Theme.palette.labelOnAccent
                        }
                    }

                    Column {
                        id: text

                        padding: 24
                        spacing: 6

                        StyledText {
                            anchors.horizontalCenter: parent.horizontalCenter
                            horizontalAlignment: Text.AlignHCenter
                            text: win.label.name ?? win.modelData.name
                            font.pointSize: 20
                            font.weight: Font.DemiBold
                        }

                        StyledText {
                            anchors.horizontalCenter: parent.horizontalCenter
                            text: win.label.mode ?? ""
                            color: Theme.palette.secondaryLabel
                        }
                    }
                }
            }
        }
    }
}
