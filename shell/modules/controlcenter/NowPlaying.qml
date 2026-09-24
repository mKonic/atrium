pragma ComponentBehavior: Bound

import QtQuick
import Atrium
import shell.components
import shell.services

// The Control Center's Now Playing page: the artwork large, the track, a
// scrubber, the controls, and the other players to switch to.
Column {
    id: root

    required property var player
    required property var players
    signal choose(var player)

    spacing: 12
    topPadding: 8
    bottomPadding: 4

    Rectangle {
        anchors.horizontalCenter: parent.horizontalCenter
        width: Math.min(root.width - 28, 220)
        height: width
        radius: 16
        color: Theme.palette.tertiaryFill
        clip: true

        Image {
            anchors.fill: parent
            source: root.player?.trackArtUrl ?? ""
            fillMode: Image.PreserveAspectCrop
            asynchronous: true
            sourceSize.width: 440
            sourceSize.height: 440
        }

        MaterialIcon {
            anchors.centerIn: parent
            visible: !(root.player?.trackArtUrl)
            text: "music_note"
            font.pointSize: 42
            color: Theme.palette.secondaryLabel
        }
    }

    Column {
        x: 14
        width: root.width - 28
        spacing: 2

        StyledText {
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            text: root.player?.trackTitle || root.player?.identity || ""
            elide: Text.ElideRight
            font.weight: Font.DemiBold
        }

        StyledText {
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            text: root.player?.subtitle ?? ""
            elide: Text.ElideRight
            font.pointSize: Theme.font.size.small
            color: Theme.palette.secondaryLabel
        }
    }

    // Scrubber: drag or click to seek, where the player allows it.
    Item {
        x: 14
        width: root.width - 28
        height: 30
        visible: (root.player?.lengthSupported ?? false) && (root.player?.length ?? 0) > 0

        readonly property real fraction: Math.max(0, Math.min(1, (root.player?.position ?? 0) / Math.max(1, root.player?.length ?? 1)))

        Rectangle {
            id: track

            width: parent.width
            height: 4
            radius: 2
            color: Theme.palette.fill

            Rectangle {
                width: parent.width * (seek.pressed ? seek.fraction : parent.parent.fraction)
                height: parent.height
                radius: 2
                color: Theme.palette.label
            }
        }

        MouseArea {
            id: seek

            readonly property real fraction: Math.max(0, Math.min(1, mouseX / width))

            anchors.fill: track
            anchors.margins: -8
            enabled: root.player?.canSeek ?? false
            cursorShape: enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
            onReleased: root.player.position = fraction * root.player.length
        }

        StyledText {
            anchors.left: parent.left
            anchors.bottom: parent.bottom
            text: SystemInfo.clockTime(seek.pressed ? seek.fraction * (root.player?.length ?? 0) : root.player?.position ?? 0)
            font.pointSize: Theme.font.size.small
            color: Theme.palette.secondaryLabel
        }

        StyledText {
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            text: "-" + SystemInfo.clockTime((root.player?.length ?? 0) - (root.player?.position ?? 0))
            font.pointSize: Theme.font.size.small
            color: Theme.palette.secondaryLabel
        }
    }

    Row {
        anchors.horizontalCenter: parent.horizontalCenter
        spacing: 18

        Repeater {
            model: [
                { icon: "skip_previous", run: () => root.player?.previous(), ok: root.player?.canGoPrevious ?? false, big: false },
                { icon: root.player?.isPlaying ? "pause" : "play_arrow", run: () => root.player?.togglePlaying(), ok: root.player?.canTogglePlaying ?? true, big: true },
                { icon: "skip_next", run: () => root.player?.next(), ok: root.player?.canGoNext ?? false, big: false }
            ]

            Rectangle {
                id: control

                required property var modelData

                width: modelData.big ? 52 : 42
                height: width
                radius: width / 2
                anchors.verticalCenter: parent?.verticalCenter
                color: controlArea.containsMouse ? Theme.palette.secondaryFill : "transparent"
                opacity: modelData.ok ? 1 : 0.35

                MaterialIcon {
                    anchors.centerIn: parent
                    text: control.modelData.icon
                    fill: 1
                    font.pointSize: control.modelData.big ? 26 : 20
                }

                MouseArea {
                    id: controlArea

                    anchors.fill: parent
                    hoverEnabled: true
                    enabled: control.modelData.ok
                    onClicked: control.modelData.run()
                }
            }
        }
    }

    // Other players, to switch to.
    Column {
        width: root.width
        visible: root.players.length > 1

        Repeater {
            model: root.players

            Rectangle {
                id: entry

                required property var modelData

                width: root.width
                height: 40
                radius: 12
                color: entry.modelData === root.player ? Theme.palette.accentFill
                     : entryArea.containsMouse ? Theme.palette.tertiaryFill : "transparent"

                MaterialIcon {
                    x: 14
                    anchors.verticalCenter: parent.verticalCenter
                    text: entry.modelData.isPlaying ? "graphic_eq" : "music_note"
                    color: entry.modelData === root.player ? Theme.palette.accent : Theme.palette.secondaryLabel
                }

                StyledText {
                    x: 44
                    width: parent.width - 58
                    anchors.verticalCenter: parent.verticalCenter
                    text: entry.modelData.identity + (entry.modelData.trackTitle ? " — " + entry.modelData.trackTitle : "")
                    elide: Text.ElideRight
                    font.pointSize: Theme.font.size.small
                }

                MouseArea {
                    id: entryArea

                    anchors.fill: parent
                    hoverEnabled: true
                    onClicked: root.choose(entry.modelData)
                }
            }
        }
    }
}
