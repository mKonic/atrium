import QtQuick
import Quickshell.Bluetooth
import Quickshell.Networking
import qs.components
import qs.services
import Atrium

// Network and Bluetooth at a glance, as the menu bar shows them on a Mac:
// wired or Wi-Fi strength, and Bluetooth while something is connected.
// Opens the Control Center.
Pill {
    id: root

    readonly property var wired: Networking.devices.values.find(d => d.type === DeviceType.Wired && d.connected) ?? null
    readonly property var wifi: Networking.devices.values.find(d => d.type === DeviceType.Wifi) ?? null
    readonly property var network: wifi?.networks.values.find(n => n.connected) ?? null
    readonly property bool limited: Networking.connectivity === NetworkConnectivity.Limited
                                    || Networking.connectivity === NetworkConnectivity.Portal
    readonly property bool bluetooth: (Bluetooth.defaultAdapter?.devices.values ?? []).some(d => d.connected)

    readonly property bool showSpeed: Atrium.settings["bar.net_speed"] ?? true

    implicitWidth: row.implicitWidth + Theme.padding.normal * 2

    NetSpeed {
        id: speed

        active: root.showSpeed && root.visible
    }

    // Room for the widest reading, so the bar does not shift as it changes.
    TextMetrics {
        id: widest

        font.family: Theme.font.sans
        font.pointSize: Theme.font.size.smaller
        text: "888 KB/s"
    }

    Row {
        id: row

        anchors.centerIn: parent
        spacing: Theme.spacing.small / 2

        MaterialIcon {
            anchors.verticalCenter: parent.verticalCenter
            text: root.wired ? "lan"
                : root.network ? (root.limited ? "wifi_find"
                    : root.network.signalStrength > 0.66 ? "wifi" : root.network.signalStrength > 0.33 ? "wifi_2_bar" : "wifi_1_bar")
                : root.wifi && Networking.wifiEnabled ? "wifi_off" : "signal_disconnected"
            font.pointSize: Theme.font.size.normal
            color: root.wired || root.network ? Theme.palette.m3OnSurface : Theme.palette.m3Outline
        }

        Rate {
            glyph: "arrow_downward"
            text: speed.downText
        }

        Rate {
            glyph: "arrow_upward"
            text: speed.upText
        }

        MaterialIcon {
            anchors.verticalCenter: parent.verticalCenter
            visible: root.bluetooth
            text: "bluetooth"
            font.pointSize: Theme.font.size.normal
        }
    }

    TapHandler {
        onTapped: Panels.toggle("control")
    }

    component Rate: Row {
        id: rate

        property string glyph
        property string text

        anchors.verticalCenter: parent.verticalCenter
        visible: root.showSpeed

        MaterialIcon {
            anchors.verticalCenter: parent.verticalCenter
            text: rate.glyph
            font.pointSize: Theme.font.size.smaller
            color: Theme.palette.m3OnSurfaceVariant
        }

        StyledText {
            anchors.verticalCenter: parent.verticalCenter
            width: widest.advanceWidth
            text: rate.text
            font.pointSize: Theme.font.size.smaller
            font.features: { "tnum": 1 }
            color: Theme.palette.m3OnSurfaceVariant
        }
    }
}
