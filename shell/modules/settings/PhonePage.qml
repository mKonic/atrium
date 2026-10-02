pragma ComponentBehavior: Bound

import QtQuick
import Atrium
import shell.components
import shell.services

// Phone: a paired phone's sound played here over the network
// (atrium-phonelink), pairing with the code both screens show, and the
// phones paired so far.
Column {
    id: root

    readonly property bool on: Atrium.settings["phone.audio"] ?? false
    readonly property string state: PhoneLink.state

    spacing: 20

    Group {
        title: "Phone Audio"
        subtitle: "Play your phone's sound here, over the network, instead of on the phone. Both need to be on the same network; the phone needs the atrium module."
        headerActions: [
            Switch {
                checked: root.on
                onToggled: Atrium.setSetting("phone.audio", !root.on)
            }
        ]

        DeviceRow {
            visible: root.on && root.state !== "confirm"
            glyph: "smartphone"
            name: PhoneLink.phone || (PhoneLink.phones.length ? "No phone nearby" : "No phone paired")
            active: root.state === "streaming"
            busy: root.state === "connecting" || root.state === "pairing"
            note: ({
                    streaming: "Playing its sound here",
                    connected: "Connected, nothing playing",
                    connecting: "Connecting…",
                    pairing: "Waiting for a phone that is pairing too…",
                    searching: PhoneLink.phones.length ? "Looking for your phone on the network" : "Pair a phone to start",
                    failed: "Its sound can't come here: " + PhoneLink.error,
                })[root.state] ?? "Not running"

            PillButton {
                visible: root.state === "pairing"
                text: "Cancel"
                onClicked: PhoneLink.cancelPairing()
            }

            PillButton {
                visible: root.state !== "pairing" && root.state !== ""
                text: "Pair"
                primary: PhoneLink.phones.length === 0
                onClicked: PhoneLink.pair()
            }
        }

        StyledText {
            visible: root.state === "pairing"
            width: parent.width
            padding: 10
            wrapMode: Text.Wrap
            text: "On the phone, open KernelSU › Modules › atrium clipboard sync, open its page and press Pair."
            color: Theme.palette.secondaryLabel
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
        visible: PhoneLink.phones.length > 0
        title: "Paired Phones"

        Repeater {
            model: PhoneLink.phones

            DeviceRow {
                id: paired

                required property var modelData

                glyph: "smartphone"
                name: modelData.name
                note: modelData.name === PhoneLink.phone && root.state === "streaming" ? "Playing" : ""

                PillButton {
                    text: "Forget"
                    onClicked: PhoneLink.forget(paired.modelData.id)
                }
            }
        }
    }

    Group {
        title: "Phone Module"

        DeviceRow {
            glyph: "download"
            name: "atrium module for rooted phones"
            note: "Install it with KernelSU, then pair."

            PillButton {
                text: "Download"
                onClicked: Qt.openUrlExternally(PhoneClipboard.moduleUrl)
            }
        }
    }
}
