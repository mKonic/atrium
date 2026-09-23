pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Effects
import Quickshell
import Quickshell.Wayland
import Quickshell.Widgets
import qs.components
import qs.services
import Atrium

// About This Computer: the system's logo and name, and what the machine is,
// read from the hardware when the shell starts.
PanelWindow {
    id: root

    property string uptime: ""

    visible: Panels.open === "about"
    onVisibleChanged: if (visible) uptime = SystemInfo.uptime()
    screen: Quickshell.screens.find(s => s.name === Atrium.focusedOutput?.name) ?? Quickshell.screens[0]
    anchors {
        top: true
        bottom: true
        left: true
        right: true
    }
    exclusiveZone: -1
    color: "transparent"
    WlrLayershell.layer: WlrLayer.Top
    WlrLayershell.namespace: "atrium-about"
    WlrLayershell.keyboardFocus: visible ? WlrKeyboardFocus.OnDemand : WlrKeyboardFocus.None

    // A click beside the card puts it away.
    MouseArea {
        anchors.fill: parent
        onClicked: Panels.open = ""
    }

    Rectangle {
        id: card

        anchors.centerIn: parent
        width: 420
        height: column.implicitHeight + 56
        radius: 26
        color: Theme.panel(Theme.palette.m3SurfaceContainer, 0.9)
        border.width: 1
        border.color: Theme.alpha(Theme.palette.m3Outline, 0.22)
        focus: true
        Keys.onEscapePressed: Panels.open = ""

        layer.enabled: true
        layer.effect: MultiEffect {
            shadowEnabled: true
            shadowColor: Qt.rgba(0, 0, 0, 0.5)
            shadowBlur: 1
            shadowVerticalOffset: 8
        }

        MouseArea {
            anchors.fill: parent  // clicks on the card stay on it
        }

        Column {
            id: column

            anchors.horizontalCenter: parent.horizontalCenter
            y: 30
            width: parent.width - 56
            spacing: 4

            IconImage {
                anchors.horizontalCenter: parent.horizontalCenter
                implicitSize: 96
                source: Quickshell.iconPath(SystemInfo.logo, "start-here")
            }

            Item {
                width: 1
                height: 10
            }

            StyledText {
                anchors.horizontalCenter: parent.horizontalCenter
                text: SystemInfo.osName
                font.pointSize: 20
                font.weight: Font.Bold
            }

            StyledText {
                anchors.horizontalCenter: parent.horizontalCenter
                text: `atrium ${SystemInfo.version}`
                font.pointSize: Theme.font.size.small
                color: Theme.palette.m3OnSurfaceVariant
            }

            Item {
                width: 1
                height: 14
            }

            Fact {
                label: "Name"
                value: SystemInfo.hostname
            }
            Fact {
                label: "Processor"
                value: SystemInfo.cpu
            }
            Fact {
                label: "Graphics"
                value: SystemInfo.gpus.join("\n")
            }
            Fact {
                label: "Memory"
                value: SystemInfo.memory
            }
            Fact {
                label: "Kernel"
                value: SystemInfo.kernel
            }
            Fact {
                label: "Uptime"
                value: root.uptime
            }
        }
    }

    // A label on the left, its value on the right, as macOS lays these out.
    component Fact: Item {
        id: fact

        property string label
        property string value

        visible: value.length > 0
        width: column.width
        height: Math.max(labelText.implicitHeight, valueText.implicitHeight) + 6

        StyledText {
            id: labelText

            width: parent.width * 0.3
            horizontalAlignment: Text.AlignRight
            text: fact.label
            font.pointSize: Theme.font.size.small
            font.weight: Font.DemiBold
        }

        StyledText {
            id: valueText

            x: parent.width * 0.3 + 12
            width: parent.width - x
            text: fact.value
            wrapMode: Text.WordWrap
            font.pointSize: Theme.font.size.small
            color: Theme.palette.m3OnSurfaceVariant
        }
    }
}
