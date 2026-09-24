pragma ComponentBehavior: Bound

import QtQuick
import shell.components
import shell.services
import Atrium

// Mouse & Touchpad, per device: each mouse or touchpad plugged in, with its
// own speed, acceleration and scrolling over the shared settings above.
// What a device doesn't set follows those.
Column {
    id: root

    spacing: 20

    Component.onCompleted: Atrium.refreshDevices()
    onVisibleChanged: if (visible) Atrium.refreshDevices()

    // One setting of one device: its name on the left, the control on the right.
    component Row_: Item {
        id: row

        property string label
        default property alias control: holder.data

        width: parent?.width ?? 0
        height: 48

        StyledText {
            x: 8
            anchors.verticalCenter: parent.verticalCenter
            text: row.label
        }

        Item {
            id: holder

            anchors.right: parent.right
            anchors.rightMargin: 8
            anchors.verticalCenter: parent.verticalCenter
            width: childrenRect.width
            height: childrenRect.height
        }
    }

    Repeater {
        model: Atrium.devices

        Group {
            id: device

            required property var modelData
            readonly property var can: modelData.can ?? {}
            readonly property bool adjustable: can.speed || can.natural_scroll || can.left_handed
            readonly property bool own: modelData.speed !== null || modelData.acceleration !== null
                                        || modelData.natural_scroll !== null || modelData.left_handed !== null

            function set(key: string, value: var): void {
                Atrium.setDevice(modelData.name, { [key]: value });
            }

            title: modelData.name
            subtitle: modelData.touchpad ? "Touchpad" : "Mouse"
            headerActions: [
                PillButton {
                    visible: device.own
                    text: "Use Shared Settings"
                    onClicked: Atrium.setDevice(device.modelData.name,
                                                { speed: null, acceleration: null, natural_scroll: null, left_handed: null })
                }
            ]

            StyledText {
                visible: !device.adjustable
                padding: 10
                width: parent.width
                wrapMode: Text.Wrap
                text: "This device has nothing to adjust here (in a session inside another one, the pointer belongs to the outer one)."
                color: Theme.palette.m3OnSurfaceVariant
            }

            Row_ {
                visible: device.can.speed ?? false
                label: "Pointer speed"

                NumberControl {
                    value: device.modelData.speed ?? Atrium.settings["pointer.speed"] ?? 0
                    min: -1
                    max: 1
                    integer: false
                    onCommitted: v => device.set("speed", v)
                }
            }

            Row_ {
                visible: device.can.speed ?? false
                label: "Acceleration"

                ChoiceControl {
                    value: device.modelData.acceleration ?? Atrium.settings["pointer.acceleration"] ?? "adaptive"
                    choices: ["adaptive", "flat"]
                    onPicked: v => device.set("acceleration", v)
                }
            }

            Row_ {
                visible: device.can.natural_scroll ?? false
                label: "Natural scrolling"

                Switch {
                    checked: device.modelData.natural_scroll
                             ?? Atrium.settings[device.modelData.touchpad ? "touchpad.natural_scroll" : "pointer.natural_scroll"] ?? true
                    onToggled: device.set("natural_scroll", !checked)
                }
            }

            Row_ {
                visible: device.can.left_handed ?? false
                label: "Left-handed"

                Switch {
                    checked: device.modelData.left_handed ?? Atrium.settings["pointer.left_handed"] ?? false
                    onToggled: device.set("left_handed", !checked)
                }
            }
        }
    }
}
