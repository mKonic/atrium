pragma ComponentBehavior: Bound

import QtCore
import QtQuick
import Atrium
import shell.components
import shell.services

// Wired and Wi-Fi: what is connected, networks to join (a password asked
// for right here), and known ones to forget. VPNs: WireGuard configurations
// imported, a provider's whole download (Mullvad's zip) as one with its
// servers.
Column {
    id: root

    readonly property var wired: Network.wired
    readonly property var networks: Network.networks
    property var joining: null
    property string error: ""
    property string vpnNote: ""
    property bool vpnFailed: false

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

                            selectionColor: Theme.palette.accent
                            selectedTextColor: Theme.palette.labelOnAccent
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

    Group {
        visible: Vpn.available
        title: "VPN"
        headerActions: [
            PillButton {
                text: Vpn.importing ? "Importing…" : "Import…"
                enabled: !Vpn.importing
                onClicked: vpnPicker.open()
            }
        ]

        StyledText {
            visible: Vpn.tunnels.length === 0
            width: parent.width
            padding: 10
            text: "Import a WireGuard configuration: a .conf file, or a provider's .zip of them, as Mullvad gives."
            wrapMode: Text.Wrap
            color: Theme.palette.secondaryLabel
        }

        Repeater {
            model: Vpn.tunnels

            DeviceRow {
                required property VpnTunnel modelData

                glyph: "vpn_key"
                name: modelData.name
                note: modelData.error ? modelData.error
                      : modelData.busy ? "Connecting…"
                      : modelData.connected ? (modelData.location ? `Connected · ${modelData.location}` : "Connected")
                      : modelData.hasServers ? `${modelData.serverCount} servers · ${modelData.location}` : "Not connected"
                active: modelData.connected
                busy: modelData.busy

                PillButton {
                    text: modelData.connected || modelData.busy ? "Disconnect" : "Connect"
                    onClicked: modelData.toggle()
                }

                PillButton {
                    text: "Remove"
                    onClicked: modelData.remove()
                }
            }
        }

        // Where turning one with servers on goes.
        Repeater {
            model: Vpn.tunnels

            ControlRow {
                required property VpnTunnel modelData

                visible: modelData.hasServers
                title: Vpn.tunnels.length > 1 ? `${modelData.name}: Default Location` : "Default Location"
                note: "Where it connects when turned on."

                Dropdown {
                    fieldWidth: 260
                    options: modelData.places
                    value: modelData.defaultPlace
                    placeholder: "Last Used"
                    onPicked: v => modelData.defaultPlace = v
                }
            }
        }

        StyledText {
            visible: root.vpnNote !== ""
            width: parent.width
            padding: 10
            text: root.vpnNote
            wrapMode: Text.Wrap
            color: root.vpnFailed ? Theme.palette.red : Theme.palette.secondaryLabel
        }
    }

    FilePicker {
        id: vpnPicker

        title: "Import a VPN Configuration"
        folder: StandardPaths.writableLocation(StandardPaths.DownloadLocation)
        nameFilter: "WireGuard configurations (*.conf *.zip)"
        onPicked: file => {
            root.vpnNote = "";
            Vpn.importFile(file);
        }
    }

    Connections {
        target: Vpn

        function onImported(name: string, servers: int): void {
            root.vpnFailed = false;
            root.vpnNote = servers > 1 ? `${name} imported, with ${servers} servers to choose from in Control Center.` : `${name} imported.`;
        }

        function onImportFailed(error: string): void {
            root.vpnFailed = true;
            root.vpnNote = `Couldn't import it: ${error}`;
        }
    }
}
