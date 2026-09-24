pragma ComponentBehavior: Bound

import QtQuick
import Atrium.Shell
import shell.components
import shell.modules.settings
import shell.services
import Atrium

// Choose what to share: a whole screen or one window, each shown as a small
// picture of itself. Double-click, or pick one and Share; closing the window
// or Cancel shares nothing.
FloatingWindow {
    id: root

    property string tab: ShareChooser.screens.length > 0 ? "screens" : "windows"
    property string chosen
    readonly property var sources: tab === "screens" ? ShareChooser.screens : ShareChooser.windows

    title: "Share Your Screen"
    color: Theme.palette.m3Surface
    implicitWidth: 760
    implicitHeight: 540
    minimumSize: Qt.size(560, 400)
    onVisibleChanged: if (!visible) ShareChooser.cancel()

    Item {
        anchors.fill: parent
        focus: true
        Keys.onEscapePressed: ShareChooser.cancel()
        Keys.onReturnPressed: if (root.chosen) ShareChooser.choose(root.chosen)

        StyledText {
            id: heading

            anchors.top: parent.top
            anchors.topMargin: 26
            anchors.horizontalCenter: parent.horizontalCenter
            text: "Choose what to share"
            font.pointSize: 18
            font.weight: Font.Bold
        }

        ChoiceControl {
            id: tabs

            anchors.top: heading.bottom
            anchors.topMargin: 14
            anchors.horizontalCenter: parent.horizontalCenter
            visible: ShareChooser.screens.length > 0 && ShareChooser.windows.length > 0
            choices: ["screens", "windows"]
            value: root.tab
            onPicked: v => {
                root.tab = v;
                root.chosen = "";
            }
        }

        GridView {
            id: grid

            readonly property int columns: Math.max(1, Math.floor(width / 230))

            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: tabs.visible ? tabs.bottom : heading.bottom
            anchors.bottom: footer.top
            anchors.margins: 24
            clip: true
            cellWidth: Math.floor(width / columns)
            cellHeight: 190
            model: root.sources

            delegate: Item {
                id: cell

                required property var modelData

                width: grid.cellWidth
                height: grid.cellHeight

                SourceCard {
                    anchors.centerIn: parent
                    source: cell.modelData
                    chosen: root.chosen === cell.modelData.line
                    onClicked: root.chosen = cell.modelData.line
                    onDoubleClicked: ShareChooser.choose(cell.modelData.line)
                }
            }
        }

        StyledText {
            anchors.centerIn: grid
            visible: root.sources.length === 0
            text: "There is nothing to share."
            color: Theme.palette.m3OnSurfaceVariant
        }

        Item {
            id: footer

            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            height: 68

            Rectangle {
                width: parent.width
                height: 1
                color: Theme.alpha(Theme.palette.m3Outline, 0.15)
            }

            Row {
                anchors.right: parent.right
                anchors.rightMargin: 24
                anchors.verticalCenter: parent.verticalCenter
                spacing: 10

                PillButton {
                    text: "Cancel"
                    onClicked: ShareChooser.cancel()
                }

                PillButton {
                    primary: true
                    enabled: root.chosen !== ""
                    opacity: enabled ? 1 : 0.5
                    text: "Share"
                    onClicked: ShareChooser.choose(root.chosen)
                }
            }
        }
    }
}
