pragma ComponentBehavior: Bound

import QtQuick
import Quickshell.Networking
import qs.components
import qs.services

// Wired and Wi-Fi: what is connected, networks to join (a password asked
// for right here), and known ones to forget.
Column {
    id: root

    readonly property var wired: Networking.devices.values.filter(d => d.type === DeviceType.Wired)
    readonly property var wifi: Networking.devices.values.find(d => d.type === DeviceType.Wifi) ?? null
    readonly property var networks: (wifi?.networks.values ?? []).slice().sort((a, b) => b.connected - a.connected || b.signalStrength - a.signalStrength)
    property var joining: null
    property string error: ""

    spacing: 20

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
        visible: root.wifi !== null
        title: "Wi-Fi"
        headerActions: [
            Switch {
                checked: Networking.wifiEnabled
                onToggled: Networking.wifiEnabled = !Networking.wifiEnabled
            }
        ]

        StyledText {
            visible: !Networking.wifiEnabled || root.networks.length === 0
            padding: 10
            text: Networking.wifiEnabled ? "No networks in range." : "Wi-Fi is off."
            color: Theme.palette.m3OnSurfaceVariant
        }

        Repeater {
            model: Networking.wifiEnabled ? root.networks.slice(0, 20) : []

            Column {
                id: net

                required property var modelData
                readonly property bool asking: root.joining === modelData

                width: parent.width

                DeviceRow {
                    glyph: net.modelData.signalStrength > 0.66 ? "network_wifi" : net.modelData.signalStrength > 0.33 ? "network_wifi_2_bar" : "network_wifi_1_bar"
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
                        color: Theme.alpha(Theme.palette.m3OnSurface, 0.07)
                        border.width: 1
                        border.color: root.error ? "#ffb4ab" : Theme.alpha(Theme.palette.m3Primary, 0.6)

                        TextInput {
                            id: password

                            anchors.fill: parent
                            anchors.leftMargin: 10
                            anchors.rightMargin: 10
                            verticalAlignment: TextInput.AlignVCenter
                            echoMode: TextInput.Password
                            color: Theme.palette.m3OnSurface
                            font.family: Theme.font.sans
                            clip: true
                            onAccepted: join.clicked()

                            StyledText {
                                anchors.verticalCenter: parent.verticalCenter
                                visible: !password.text
                                text: root.error || "Password"
                                color: root.error ? "#ffb4ab" : Theme.alpha(Theme.palette.m3OnSurfaceVariant, 0.6)
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
