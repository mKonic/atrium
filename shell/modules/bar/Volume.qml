import QtQuick
import Quickshell.Services.Pipewire
import qs.components
import qs.services

// The default output's volume: scroll to change it, click to mute.
Pill {
    id: root

    readonly property PwNode sink: Pipewire.defaultAudioSink
    readonly property real volume: sink?.audio?.volume ?? 0
    readonly property bool muted: sink?.audio?.muted ?? false

    visible: sink !== null
    implicitWidth: row.implicitWidth + Theme.padding.normal * 2

    PwObjectTracker {
        objects: [root.sink]
    }

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
            if (root.sink?.audio)
                root.sink.audio.muted = !root.sink.audio.muted;
        }
        onWheel: event => {
            if (!root.sink?.audio)
                return;
            const step = event.angleDelta.y > 0 ? 0.05 : -0.05;
            root.sink.audio.volume = Math.max(0, Math.min(1.5, root.sink.audio.volume + step));
        }
    }
}
