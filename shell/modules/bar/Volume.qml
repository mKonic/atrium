import QtQuick
import Atrium
import shell.components
import shell.services

// The default output's volume: scroll to change it, click to mute.
Pill {
    id: root

    readonly property AudioNode sink: Audio.sink
    readonly property real volume: sink?.volume ?? 0
    readonly property bool muted: sink?.muted ?? false

    visible: sink !== null
    implicitWidth: row.implicitWidth + Theme.padding.normal * 2


    Row {
        id: row

        anchors.centerIn: parent
        spacing: Theme.spacing.small / 2

        MaterialIcon {
            anchors.verticalCenter: parent.verticalCenter
            text: root.muted ? "volume_off" : root.volume >= 0.5 ? "volume_up" : root.volume > 0 ? "volume_down" : "volume_mute"
            font.pointSize: Theme.font.size.normal
            color: root.muted ? Theme.palette.m3Outline : Theme.palette.m3OnSurface
        }

        StyledText {
            anchors.verticalCenter: parent.verticalCenter
            text: `${Math.round(root.volume * 100)}%`
            font.pointSize: Theme.font.size.smaller
            color: Theme.palette.m3OnSurfaceVariant
        }
    }

    MouseArea {
        anchors.fill: parent
        onClicked: {
            if (root.sink)
                root.sink.muted = !root.sink.muted;
        }
        onWheel: event => {
            if (!root.sink)
                return;
            // The media keys' steps; a touchpad's small scrolls take the fine ones.
            // Never past 100%: boosting past full gain only clips.
            root.sink.volume = Levels.step(root.sink.volume, event.angleDelta.y > 0 ? 1 : -1, Math.abs(event.angleDelta.y) < 120);
        }
    }
}
