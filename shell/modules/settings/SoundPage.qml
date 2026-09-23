pragma ComponentBehavior: Bound

import QtQuick
import Quickshell.Services.Pipewire
import qs.components
import qs.services

// Where sound goes and comes from, how loud, and each app's own volume.
Column {
    id: root

    readonly property var devices: Pipewire.nodes.values.filter(n => n.audio && !n.isStream)
    readonly property var outputs: devices.filter(n => n.isSink)
    readonly property var inputs: devices.filter(n => !n.isSink)
    readonly property var apps: Pipewire.nodes.values.filter(n => n.audio && n.isStream && n.isSink)
    readonly property PwNode sink: Pipewire.defaultAudioSink
    readonly property PwNode source: Pipewire.defaultAudioSource

    spacing: 20

    PwObjectTracker {
        objects: root.devices.concat(root.apps)
    }

    function nameOf(n: var): string {
        return n?.description || n?.nickname || n?.name || "Unknown";
    }

    Group {
        title: "Output"

        Repeater {
            model: root.outputs

            DeviceRow {
                required property var modelData

                glyph: /hdmi|display|dp/i.test(modelData.name) ? "tv" : /headset|headphone|usb/i.test(root.nameOf(modelData)) ? "headphones" : "speaker"
                name: root.nameOf(modelData)
                note: modelData === root.sink ? "In use" : ""
                active: modelData === root.sink
                onClicked: Pipewire.preferredDefaultAudioSink = modelData
            }
        }

        LevelRow {
            node: root.sink
            label: "Output volume"
        }
    }

    Group {
        title: "Input"

        Repeater {
            model: root.inputs

            DeviceRow {
                required property var modelData

                glyph: "mic"
                name: root.nameOf(modelData)
                note: modelData === root.source ? "In use" : ""
                active: modelData === root.source
                onClicked: Pipewire.preferredDefaultAudioSource = modelData
            }
        }

        LevelRow {
            node: root.source
            label: "Input volume"
        }
    }

    Group {
        visible: root.apps.length > 0
        title: "Apps"
        subtitle: "Each app's own volume, on top of the output's."

        Repeater {
            model: root.apps

            LevelRow {
                required property var modelData

                node: modelData
                label: modelData.properties?.["application.name"] ?? root.nameOf(modelData)
            }
        }
    }

    // A volume slider with mute, for a device or an app.
    component LevelRow: Item {
        id: level

        property var node
        property string label

        visible: node?.audio !== undefined && node?.audio !== null
        width: parent?.width ?? 0
        height: 48

        MaterialIcon {
            id: mute

            x: 14
            anchors.verticalCenter: parent.verticalCenter
            text: level.node?.audio?.muted ? "volume_off" : "volume_up"
            color: level.node?.audio?.muted ? Theme.palette.m3Outline : Theme.palette.m3OnSurface

            MouseArea {
                anchors.fill: parent
                anchors.margins: -6
                onClicked: level.node.audio.muted = !level.node.audio.muted
            }
        }

        StyledText {
            id: text

            anchors.left: mute.right
            anchors.leftMargin: 12
            anchors.verticalCenter: parent.verticalCenter
            width: 170
            text: level.label
            elide: Text.ElideRight
            font.pointSize: Theme.font.size.small
        }

        NumberControl {
            anchors.left: text.right
            anchors.leftMargin: 8
            anchors.right: parent.right
            anchors.rightMargin: 12
            anchors.verticalCenter: parent.verticalCenter
            min: 0
            max: 100
            value: Math.round((level.node?.audio?.volume ?? 0) * 100)
            onCommitted: v => level.node.audio.volume = v / 100
        }
    }
}
