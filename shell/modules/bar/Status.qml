import QtQuick
import Atrium
import shell.components
import shell.services

// Network and Bluetooth at a glance, as the menu bar shows them on a Mac:
// wired or Wi-Fi strength, and Bluetooth while something is connected.
// Opens the Control Center.
Pill {
    id: root

    readonly property bool bluetooth: (Bluetooth.adapter?.connectedNames ?? "") !== ""

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
            text: Network.glyph
            font.pointSize: Theme.font.size.normal
            color: Network.online ? Theme.palette.m3OnSurface : Theme.palette.m3Outline
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
