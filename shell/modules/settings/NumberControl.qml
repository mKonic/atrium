import QtQuick
import shell.components
import shell.services

// A slider with its value beside it. The setting changes when the handle is
// let go (each change is saved); the number follows the drag.
Item {
    id: root

    property real value: 0
    property real min: 0
    property real max: 100
    property bool integer: true
    property real shown: value  // while dragging
    signal committed(var value)
    // Each step while dragging, for settings that show the change live.
    signal moved(var value)

    onShownChanged: if (drag.pressed) moved(shown)

    readonly property real fraction: max > min ? (shown - min) / (max - min) : 0

    function snap(v: real): real {
        v = Math.max(min, Math.min(max, v));
        return integer ? Math.round(v) : Math.round(v * 100) / 100;
    }

    onValueChanged: if (!drag.pressed) shown = value

    implicitWidth: 260
    implicitHeight: 28

    Rectangle {
        id: track

        anchors.left: parent.left
        anchors.right: label.left
        anchors.rightMargin: 12
        anchors.verticalCenter: parent.verticalCenter
        height: 6
        radius: 3
        color: Theme.palette.fill

        Rectangle {
            width: Math.max(0, Math.min(1, root.fraction)) * parent.width
            height: parent.height
            radius: parent.radius
            color: Theme.palette.accent
        }

        Thumb {
            x: Math.max(0, Math.min(1, root.fraction)) * parent.width - width / 2
            anchors.verticalCenter: parent.verticalCenter
            width: 18
            height: 18
        }

        MouseArea {
            id: drag

            anchors.fill: parent
            anchors.margins: -10
            function at(x: real): real {
                return root.snap(root.min + (x - 10) / track.width * (root.max - root.min));
            }
            onPressed: event => root.shown = at(event.x)
            onPositionChanged: event => root.shown = at(event.x)
            onReleased: {
                if (root.shown !== root.value)
                    root.committed(root.shown);
            }
        }
    }

    StyledText {
        id: label

        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        width: 44
        horizontalAlignment: Text.AlignRight
        text: root.integer ? String(Math.round(root.shown)) : root.shown.toFixed(2)
        font.features: { "tnum": 1 }
        color: Theme.palette.secondaryLabel
    }
}
