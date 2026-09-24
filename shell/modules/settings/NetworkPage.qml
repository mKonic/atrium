pragma ComponentBehavior: Bound

import QtQuick
import Atrium
import shell.components
import shell.services

// Wired and Wi-Fi: what is connected, networks to join (a password asked
// for right here), and known ones to forget.
Column {
    id: root

    readonly property var wired: Network.wired
    readonly property var networks: Network.networks
    property var joining: null
    property string error: ""

    spacing: 20

    MissingNote {
        needs: "networkmanager"
        explanation: "Wired and Wi-Fi connections are set up through NetworkManager."
    }

    MissingNote {
        message: Network.available && !Network.hasWifi && root.wired.length === 0 ? "No network adapters found." : ""
    }

    // Looking for networks only while the page is open.
    Binding {
        when: root.visible && Network.wifiEnabled
        target: Network
        property: "scanning"
        value: true
        restoreMode: Binding.RestoreValue
    }

    Group {
        visible: root.wired.length > 0
        title: "Ethernet"

        Repeater {
            model: root.wired

            DeviceRow {
                required property var modelData

                glyph: "lan"
                name: modelData.name
                note: modelData.connected ? "Connected" : "Not connected"
                active: modelData.connected
            }
        }
    }

    Group {
        visible: Network.hasWifi
        title: "Wi-Fi"
        headerActions: [
            Switch {
                checked: Network.wifiEnabled
                onToggled: Network.wifiEnabled = !Network.wifiEnabled
            }
        ]

        StyledText {
            visible: !Network.wifiEnabled || root.networks.length === 0
            padding: 10
            text: Network.wifiEnabled ? "No networks in range." : "Wi-Fi is off."
            color: Theme.palette.secondaryLabel
        }

        Repeater {
            model: Network.wifiEnabled ? root.networks : []

            Column {
                id: net

                required property var modelData
                readonly property bool asking: root.joining === modelData

                width: parent.width

                DeviceRow {
                    glyph: net.modelData.glyph
                    name: net.modelData.name
                    note: net.modelData.connected ? "Connected" : net.modelData.known ? "Known" : net.modelData.security > 0 ? "Secured" : "Open"
                    active: net.modelData.connected
                    busy: net.modelData.stateChanging
                    onClicked: {
                        const n = net.modelData;
                        if (n.connected)
                            return;
                        if (n.known || n.security === 0)
                            n.connect();
                        else {
                            root.error = "";
                            root.joining = net.asking ? null : n;
                        }
                    }

                    PillButton {
                        visible: net.modelData.connected
                        text: "Disconnect"
                        onClicked: net.modelData.disconnect()
                    }

                    PillButton {
                        visible: net.modelData.known && !net.modelData.connected
                        text: "Forget"
                        onClicked: net.modelData.forget()
                    }
                }

                // The password, asked for right under the network.
                Row {
                    visible: net.asking
                    x: 52
                    spacing: 8
                    bottomPadding: 8

                    Rectangle {
                        width: 240
                        height: 30
                        radius: 8
                        color: Theme.palette.tertiaryFill
                        border.width: 1
                        border.color: root.error ? Theme.palette.red : Theme.palette.focusRing

                        TextInput {
                            id: password

                            anchors.fill: parent
                            anchors.leftMargin: 10
                            anchors.rightMargin: 10
                            verticalAlignment: TextInput.AlignVCenter
                            echoMode: TextInput.Password
                            color: Theme.palette.label
                            font.family: Theme.font.sans
                            clip: true
                            onAccepted: join.clicked()

                            StyledText {
                                anchors.verticalCenter: parent.verticalCenter
                                visible: !password.text
                                text: root.error || "Password"
                                color: root.error ? Theme.palette.red : Theme.palette.tertiaryLabel
                                font.pointSize: Theme.font.size.small
                            }
                        }
                    }

                    PillButton {
                        id: join

                        text: "Join"
                        primary: true
                        enabled: password.text.length > 0
                        onClicked: {
                            root.error = "";
                            net.modelData.connectWithPsk(password.text);
                        }
                    }

                    Connections {
                        target: net.asking ? net.modelData : null

                        function onConnectionFailed(reason: var): void {
                            root.error = "Wrong password";
                        }

                        function onConnectedChanged(): void {
                            if (net.modelData.connected)
                                root.joining = null;
                        }
                    }
                }
            }
        }
    }
}
