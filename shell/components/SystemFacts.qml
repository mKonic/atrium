pragma ComponentBehavior: Bound

import QtQuick
import Atrium.Shell
import shell.services
import Atrium

// The system's logo and name, and what the machine is, read from the
// hardware: the About card and the Settings app's About page.
Column {
    id: column

    property string uptime: SystemInfo.uptime()

    spacing: 4

    IconImage {
        anchors.horizontalCenter: parent.horizontalCenter
        implicitSize: 96
        source: Shell.iconPath(SystemInfo.logo, "start-here")
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
        color: Theme.palette.secondaryLabel
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
        value: column.uptime
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
            color: Theme.palette.secondaryLabel
        }
    }
}
