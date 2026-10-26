pragma ComponentBehavior: Bound

import QtQuick
import Atrium.Shell
import shell.components
import shell.services
import Atrium

// The windows open now, each a small picture of itself as the share picker
// shows them, to choose one instead of typing its app id. A click picks.
Flow {
    id: root

    signal picked(var window)
    // Only windows with an app id (what an app's record is keyed by).
    property bool needsAppId: false

    // Apps' own windows (not dialogs, nor atrium's).
    readonly property var windows: Atrium.windows.filter(w => !w.parent && !w.skip_taskbar && !w.app_id.startsWith("atrium")
                                                         && (!needsAppId || w.app_id !== ""))

    // A pattern matching exactly this text.
    function exactly(text: string): string {
        return "^" + text.replace(/[.*+?^${}()|[\]\\]/g, "\\$&") + "$";
    }

    function refresh(): void {
        for (const w of windows)
            shots.take(w.identifier);
    }

    spacing: 12
    onVisibleChanged: if (visible) refresh()
    Component.onCompleted: if (visible) refresh()

    WindowShots {
        id: shots
    }

    StyledText {
        visible: root.windows.length === 0
        text: qsTr("No windows are open.")
        color: Theme.palette.secondaryLabel
    }

    Repeater {
        model: root.windows

        Item {
            id: card

            required property var modelData
            readonly property string shot: shots.shots[modelData.identifier] ?? ""
            // The window's own icon (a file atrium saved, or a theme name), else its app's.
            readonly property string icon: !modelData.icon ? Icons.appIcon(modelData.app_id)
                : modelData.icon.startsWith("/") ? "file://" + modelData.icon
                : Shell.iconPath(modelData.icon, "application-x-executable")

            width: 168
            height: 150

            Accessible.role: Accessible.Button
            Accessible.name: modelData.title || modelData.app_id

            Rectangle {
                id: frame

                width: parent.width
                height: 96
                radius: 10
                color: Theme.palette.tertiaryFill
                border.width: area.containsMouse ? 2 : 1
                border.color: area.containsMouse ? Theme.palette.accent : Theme.palette.separator

                Image {
                    id: picture

                    anchors.fill: parent
                    anchors.margins: 5
                    source: card.shot
                    fillMode: Image.PreserveAspectFit
                    asynchronous: true
                    smooth: true
                    mipmap: true
                }

                // Its icon while there's no picture.
                IconImage {
                    anchors.centerIn: parent
                    visible: picture.status !== Image.Ready
                    implicitSize: 40
                    source: card.icon
                }
            }

            Row {
                id: name

                anchors.top: frame.bottom
                anchors.topMargin: 6
                width: parent.width
                spacing: 6

                IconImage {
                    id: badge

                    anchors.verticalCenter: parent.verticalCenter
                    implicitSize: 16
                    source: card.icon
                }

                StyledText {
                    anchors.verticalCenter: parent.verticalCenter
                    width: parent.width - badge.width - 6
                    elide: Text.ElideRight
                    text: card.modelData.title || qsTr("Untitled")
                    font.pointSize: Theme.font.size.small
                    font.weight: Font.Medium
                }
            }

            StyledText {
                anchors.top: name.bottom
                anchors.topMargin: 2
                width: parent.width
                elide: Text.ElideMiddle
                text: card.modelData.app_id || qsTr("No app id")
                font.pointSize: Theme.font.size.smaller
                color: Theme.palette.secondaryLabel
            }

            MouseArea {
                id: area

                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: root.picked(card.modelData)
            }
        }
    }
}
