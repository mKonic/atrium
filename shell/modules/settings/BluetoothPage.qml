pragma ComponentBehavior: Bound

import QtQuick
import Atrium.Shell
import Atrium
import shell.components
import shell.services

// Bluetooth: your devices (connect, disconnect, forget) and, while this page
// is open, the ones nearby to pair.
Column {
    id: root

    readonly property BluetoothAdapter adapter: Bluetooth.adapter
    readonly property var mine: adapter?.mine ?? []
    readonly property var nearby: adapter?.nearby ?? []

    spacing: 20

    // Looking for devices only while the page is open.
    Binding {
        when: root.visible && (root.adapter?.enabled ?? false)
        target: root.adapter
        property: "discovering"
        value: true
        restoreMode: Binding.RestoreValue
    }

    Group {
        visible: root.adapter !== null
        title: "Bluetooth"
        subtitle: root.adapter ? `This computer is ${root.adapter.name}.` : ""
        headerActions: [
            Switch {
                checked: root.adapter?.enabled ?? false
                onToggled: root.adapter.enabled = !root.adapter.enabled
            }
        ]

        StyledText {
            visible: root.mine.length === 0
            padding: 10
            text: root.adapter?.enabled ? "No devices yet." : "Bluetooth is off."
            color: Theme.palette.m3OnSurfaceVariant
        }

        Repeater {
            model: root.adapter?.enabled ? root.mine : []

            DeviceRow {
                required property var modelData

                glyph: modelData.glyph
                name: modelData.name || modelData.deviceName
                note: modelData.connected ? "Connected" + (modelData.batteryAvailable ? ` · ${Math.round(modelData.battery * 100)}%` : "") : "Not connected"
                active: modelData.connected
                busy: modelData.state === BluetoothDeviceState.Connecting || modelData.state === BluetoothDeviceState.Disconnecting

                PillButton {
                    text: modelData.connected ? "Disconnect" : "Connect"
                    onClicked: modelData.connected ? modelData.disconnect() : modelData.connect()
                }

                PillButton {
                    text: "Forget"
                    onClicked: modelData.forget()
                }
            }
        }
    }

    Group {
        visible: root.adapter?.enabled ?? false
        title: "Nearby"
        subtitle: root.adapter?.discovering ? "Looking for devices…" : ""

        StyledText {
            visible: root.nearby.length === 0
            padding: 10
            text: "Nothing found yet. Put the device in pairing mode."
            color: Theme.palette.m3OnSurfaceVariant
        }

        Repeater {
            model: root.nearby

            DeviceRow {
                id: device

                required property var modelData

                glyph: modelData.glyph
                name: modelData.name
                busy: modelData.pairing

                PillButton {
                    text: device.modelData.pairing ? "Cancel" : "Pair"
                    primary: !device.modelData.pairing
                    onClicked: device.modelData.pairing ? device.modelData.cancelPair() : device.modelData.pair()
                }
            }
        }
    }
}
