pragma ComponentBehavior: Bound

import QtQuick
import Atrium
import shell.components
import shell.services

// Phone: a phone's sound played here over the network (atrium-phonelink),
// laid out like Bluetooth: your phones (connect, disconnect, connect by
// themselves, forget), the code to compare while pairing, and the phones
// nearby to pair.
Column {
    id: root

    readonly property bool on: Atrium.settings["phone.audio"] ?? false
    readonly property string state: PhoneLink.state

    spacing: 20

    Group {
        title: "Phone Audio"
        subtitle: "Play your phone's sound here, over the network, instead of on the phone. Both need to be on the same network; the phone needs Atrium Link."
        headerActions: [
            Switch {
                name: "Phone Audio"
                checked: root.on
                onToggled: Atrium.setSetting("phone.audio", !root.on)
            }
        ]

        StyledText {
            visible: root.state === ""
            padding: 10
            text: "Not running."
            color: Theme.palette.secondaryLabel
        }

        StyledText {
            visible: root.on && root.state !== "confirm" && PhoneLink.phones.length === 0
            padding: 10
            text: "No phone paired yet. Pair yours under Nearby."
            color: Theme.palette.secondaryLabel
        }

        Repeater {
            model: root.on && root.state !== "confirm" ? PhoneLink.phones : []

            Column {
                id: phone

                required property var modelData
                readonly property bool linked: ["streaming", "connected", "connecting", "failed"].includes(modelData.state)

                width: parent?.width ?? 0

                DeviceRow {
                    glyph: "smartphone"
                    name: phone.modelData.name
                    active: phone.modelData.state === "streaming"
                    busy: phone.modelData.state === "connecting"
                    note: ({
                            streaming: "Playing its sound here",
                            connected: "Connected, nothing playing",
                            connecting: "Connecting…",
                            failed: "Its sound can't come here: " + phone.modelData.error,
                            disconnected: "Not connected",
                            away: "Not on this network",
                            forgot: "It forgot this computer. Forget it here too, then pair again.",
                        })[phone.modelData.state] ?? ""

                    PillButton {
                        visible: phone.modelData.state !== "forgot"
                        text: phone.linked ? "Disconnect" : "Connect"
                        onClicked: phone.linked ? PhoneLink.disconnectPhone(phone.modelData.id) : PhoneLink.connectPhone(phone.modelData.id)
                    }

                    PillButton {
                        text: "Forget"
                        onClicked: PhoneLink.forget(phone.modelData.id)
                    }
                }

                ControlRow {
                    title: "Connect automatically"
                    note: "Whenever it's on this network."

                    Switch {
                        name: "Connect automatically"
                        checked: phone.modelData.auto
                        onToggled: PhoneLink.setAutoConnect(phone.modelData.id, !phone.modelData.auto)
                    }
                }
            }
        }

        // Numeric comparison: the same code on both screens means no one
        // is in between.
        Column {
            visible: root.on && root.state === "confirm"
            width: parent.width
            spacing: 8
            padding: 10

            StyledText {
                text: `Pairing with ${PhoneLink.phone || "a phone"}`
            }

            StyledText {
                text: PhoneLink.code
                font.family: Theme.font.mono
                font.pointSize: 28
                font.letterSpacing: 6
            }

            StyledText {
                text: "Accept if the phone shows the same code, and accept there too."
                color: Theme.palette.secondaryLabel
            }

            Row {
                spacing: 8

                PillButton {
                    text: "Accept"
                    primary: true
                    onClicked: PhoneLink.accept()
                }

                PillButton {
                    text: "Reject"
                    onClicked: PhoneLink.reject()
                }
            }
        }
    }

    Group {
        visible: root.on && root.state !== ""
        title: "Nearby"
        subtitle: "On the phone, open Atrium Link and press Pair, then pair it here."

        StyledText {
            visible: PhoneLink.nearby.length === 0
            padding: 10
            text: "No other phone with Atrium Link on this network."
            color: Theme.palette.secondaryLabel
        }

        Repeater {
            model: PhoneLink.nearby

            DeviceRow {
                id: device

                required property var modelData
                readonly property bool pairing: modelData.state === "pairing"

                glyph: "smartphone"
                name: modelData.name
                busy: pairing
                note: pairing ? "Pairing…" : modelData.error || (modelData.ready ? "Ready to pair" : "Press Pair on the phone first")

                PillButton {
                    text: device.pairing ? "Cancel" : "Pair"
                    primary: !device.pairing && device.modelData.ready
                    onClicked: device.pairing ? PhoneLink.cancelPairing() : PhoneLink.pairWith(device.modelData.id)
                }
            }
        }
    }

    Group {
        title: "Atrium Link"

        DeviceRow {
            glyph: "download"
            name: "Atrium Link for Android"
            note: "For rooted phones: install the module with KernelSU, then pair in the Atrium Link app."

            PillButton {
                text: "Download"
                onClicked: Qt.openUrlExternally(PhoneClipboard.moduleUrl)
            }
        }
    }
}
