import QtQuick
import qs.services

// A thick rounded slider with its icon inside, like macOS's Control Center.
// `value` is 0..1; `moved` fires while dragging, `released` at the end.
Item {
    id: root

    property real value: 0
    property string icon: ""
    property bool enabled_: true
    signal moved(real value)
    signal released(real value)

    implicitHeight: 34

    Rectangle {
        id: track

        anchors.fill: parent
        radius: height / 2
        color: Theme.palette.m3SurfaceContainerHigh
        clip: true

        Rectangle {
            width: Math.max(track.height, track.width * Math.max(0, Math.min(1, root.value)))
            height: parent.height
            radius: height / 2
            color: root.enabled_ ? Theme.palette.m3Primary : Theme.palette.m3Outline

            Behavior on width {
                enabled: !drag.pressed
                Anim {
                    duration: Theme.anim.small
                }
            }
        }

        MaterialIcon {
            anchors.left: parent.left
            anchors.leftMargin: 10
            anchors.verticalCenter: parent.verticalCenter
            text: root.icon
            fill: 1
            font.pointSize: Theme.font.size.normal
            color: root.value > 0.08 ? Theme.palette.m3OnPrimary : Theme.palette.m3OnSurfaceVariant
        }
    }

    MouseArea {
        id: drag

        anchors.fill: parent
        enabled: root.enabled_

        function valueAt(x: real): real {
            return Math.max(0, Math.min(1, x / width));
        }

        onPressed: event => root.moved(valueAt(event.x))
        onPositionChanged: event => {
            if (pressed)
                root.moved(valueAt(event.x));
        }
        onReleased: event => root.released(valueAt(event.x))
        onWheel: event => {
            const v = Math.max(0, Math.min(1, root.value + (event.angleDelta.y > 0 ? 0.05 : -0.05)));
            root.moved(v);
            root.released(v);
        }
    }
}
