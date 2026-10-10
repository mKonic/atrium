pragma ComponentBehavior: Bound

import QtQuick
import Atrium.Shell
import Atrium
import shell.components
import shell.services

// Bluetooth: your devices (connect, disconnect, forget), the clipboard
// shared with a phone and, while this page is open, the ones nearby to pair.
Column {
    id: root

    readonly property BluetoothAdapter adapter: Bluetooth.adapter
    readonly property var mine: adapter?.mine ?? []
    readonly property var nearby: adapter?.nearby ?? []

    spacing: 20

    MissingNote {
        needs: "bluez"
        explanation: qsTr("Bluetooth devices are paired and connected through BlueZ (bluetooth.service).")
    }

    MissingNote {
        needs: "bluetooth-adapter"
    }

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
        title: qsTr("Bluetooth")
        subtitle: root.adapter ? qsTr("This computer is %1.").arg(root.adapter.name) : ""
        headerActions: [
            Switch {
                name: qsTr("Bluetooth")
                checked: root.adapter?.enabled ?? false
                onToggled: root.adapter.enabled = !root.adapter.enabled
            }
        ]

        StyledText {
            visible: root.mine.length === 0
            padding: 10
            text: root.adapter?.enabled ? qsTr("No devices yet.") : qsTr("Bluetooth is off.")
            color: Theme.palette.secondaryLabel
        }

        Repeater {
            model: root.adapter?.enabled ? root.mine : []

            DeviceRow {
                required property var modelData

                glyph: modelData.glyph
                name: modelData.name || modelData.deviceName
                note: modelData.connected ? qsTr("Connected") + (modelData.batteryAvailable ? ` · ${Math.round(modelData.battery * 100)}%` : "") : qsTr("Not connected")
                active: modelData.connected
                busy: modelData.state === BluetoothDeviceState.Connecting || modelData.state === BluetoothDeviceState.Disconnecting

                PillButton {
                    text: modelData.connected ? qsTr("Disconnect") : qsTr("Connect")
                    onClicked: modelData.connected ? modelData.disconnect() : modelData.connect()
                }

                PillButton {
                    text: qsTr("Forget")
                    onClicked: modelData.forget()
                }
            }
        }
    }

    Group {
        id: phoneClipboard

        readonly property bool on: Atrium.settings["bluetooth.phone_clipboard"] ?? false
        readonly property string state: PhoneClipboard.state
        readonly property string phone: PhoneClipboard.phone

        visible: root.adapter !== null
        title: qsTr("Phone Clipboard")
        subtitle: qsTr("Share the clipboard with your phone while it is connected: the last few copies when it connects, then every copy on either side.")
        headerActions: [
            Switch {
                name: qsTr("Phone Clipboard")
                checked: phoneClipboard.on
                onToggled: Atrium.setSetting("bluetooth.phone_clipboard", !phoneClipboard.on)
            }
        ]

        DeviceRow {
            visible: phoneClipboard.on
            glyph: "smartphone"
            name: phoneClipboard.phone || "No phone connected"
            active: phoneClipboard.state === "connected"
            busy: phoneClipboard.state === "connecting"
            note: ({
                    connected: "Sharing the clipboard",
                    connecting: "Connecting…",
                    missing: "Connected, but its clipboard module isn't answering",
                    waiting: "Connect your phone to share its clipboard",
                    unavailable: "Bluetooth is off",
                })[phoneClipboard.state] ?? "Not running"
        }

        DeviceRow {
            glyph: "download"
            name: "Atrium Link for Android"
            note: qsTr("For rooted phones: install the module with KernelSU, then connect the phone.")

            PillButton {
                text: qsTr("Download")
                onClicked: Qt.openUrlExternally(PhoneClipboard.moduleUrl)
            }
        }

        StyledText {
            padding: 10
            text: qsTr("Phones without root: coming soon.")
            color: Theme.palette.secondaryLabel
        }
    }

    Group {
        visible: root.adapter?.enabled ?? false
        title: qsTr("Nearby")
        subtitle: root.adapter?.discovering ? qsTr("Looking for devices…") : ""

        StyledText {
            visible: root.nearby.length === 0
            padding: 10
            text: qsTr("Nothing found yet. Put the device in pairing mode.")
            color: Theme.palette.secondaryLabel
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
                    text: device.modelData.pairing ? qsTr("Cancel") : qsTr("Pair")
                    primary: !device.modelData.pairing
                    onClicked: device.modelData.pairing ? device.modelData.cancelPair() : device.modelData.pair()
                }
            }
        }
    }
}
