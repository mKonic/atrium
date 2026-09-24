pragma ComponentBehavior: Bound

import QtQuick
import Atrium
import shell.components
import shell.services

// Where sound goes and comes from, how loud, and each app's own volume.
Column {
    id: root

    readonly property AudioNode sink: Audio.sink
    readonly property AudioNode source: Audio.source

    spacing: 20

    MissingNote {
        id: missing

        needs: "pipewire"
        explanation: "Sound devices and volumes come from PipeWire (pipewire-pulse and WirePlumber)."
    }

    Group {
        visible: !missing.visible
        title: "Output"

        Repeater {
            model: Audio.outputs

            DeviceRow {
                required property var modelData

                glyph: modelData.glyph
                name: modelData.label
                note: modelData.isDefault ? "In use" : ""
                active: modelData.isDefault
                onClicked: modelData.makeDefault()
            }
        }

        LevelRow {
            node: root.sink
            label: "Output volume"
        }
    }

    Group {
        visible: !missing.visible
        title: "Input"

        Repeater {
            model: Audio.inputs

            DeviceRow {
                required property var modelData

                glyph: modelData.glyph
                name: modelData.label
                note: modelData.isDefault ? "In use" : ""
                active: modelData.isDefault
                onClicked: modelData.makeDefault()
            }
        }

        LevelRow {
            node: root.source
            label: "Input volume"
        }
    }

    Group {
        visible: !missing.visible && Audio.apps.length > 0
        title: "Apps"
        subtitle: "Each app's own volume, on top of the output's."

        Repeater {
            model: Audio.apps

            LevelRow {
                required property var modelData

                node: modelData
                label: modelData.label
            }
        }
    }

    // A volume slider with mute, for a device or an app.
    component LevelRow: Item {
        id: level

        property var node
        property string label

        visible: node !== undefined && node !== null
        width: parent?.width ?? 0
        height: 48

        MaterialIcon {
            id: mute

            x: 14
            anchors.verticalCenter: parent.verticalCenter
            text: level.node?.muted ? "volume_off" : "volume_up"
            color: level.node?.muted ? Theme.palette.tertiaryLabel : Theme.palette.label

            MouseArea {
                anchors.fill: parent
                anchors.margins: -6
                onClicked: level.node.muted = !level.node.muted
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
            value: Math.round((level.node?.volume ?? 0) * 100)
            onCommitted: v => level.node.volume = v / 100
        }
    }
}
