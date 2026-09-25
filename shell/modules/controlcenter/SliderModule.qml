import QtQuick
import shell.components
import shell.services

// Display or Sound, as macOS 26's Control Center has them: the name, then a
// thin track with a white fill between a small and a large icon, dragged
// anywhere along it. `value` is 0..1.
Module {
    id: root

    property string title
    property string lowIcon
    property string highIcon
    property real value
    signal moved(real value)
    signal released(real value)

    radius: 26
    implicitHeight: 76

    StyledText {
        x: 16
        y: 12
        text: root.title
        font.weight: Font.DemiBold
        font.pointSize: Theme.font.size.smaller
    }

    MaterialIcon {
        id: low

        x: 14
        y: 44
        text: root.lowIcon
        font.pointSize: Theme.font.size.normal
        color: Theme.palette.label
    }

    MaterialIcon {
        id: high

        anchors.right: parent.right
        anchors.rightMargin: 14
        anchors.verticalCenter: low.verticalCenter
        text: root.highIcon
        font.pointSize: Theme.font.size.larger
        color: Theme.palette.label
    }

    Rectangle {
        id: track

        anchors.left: low.right
        anchors.leftMargin: 10
        anchors.right: high.left
        anchors.rightMargin: 10
        anchors.verticalCenter: low.verticalCenter
        height: 6
        radius: 3
        color: Theme.palette.fill

        Rectangle {
            width: track.width * Math.max(0, Math.min(1, root.value))
            height: parent.height
            radius: 3
            color: "white"
        }
    }

    MouseArea {
        anchors.left: track.left
        anchors.right: track.right
        anchors.verticalCenter: track.verticalCenter
        anchors.leftMargin: -8
        anchors.rightMargin: -8
        height: 28
        cursorShape: Qt.PointingHandCursor
        preventStealing: true
        function at(x: real): real {
            return Math.max(0, Math.min(1, (x - 8) / track.width));
        }
        onPressed: event => root.moved(at(event.x))
        onPositionChanged: event => { if (pressed) root.moved(at(event.x)); }
        onReleased: event => root.released(at(event.x))
    }
}
