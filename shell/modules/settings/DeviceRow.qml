import QtQuick
import shell.components
import shell.services

// A device or network in a list: a round badge (filled when it is the one
// in use), its name and a note, and room for buttons at the end.
Item {
    id: root

    property string glyph
    property string name
    property string note
    property bool active: false
    property bool busy: false
    default property alias actions: buttons.data
    signal clicked

    width: parent?.width ?? 0
    height: 48

    Rectangle {
        anchors.fill: parent
        radius: 10
        color: area.containsMouse ? Theme.alpha(Theme.palette.m3OnSurface, 0.05) : "transparent"
    }

    Rectangle {
        id: badge

        x: 8
        anchors.verticalCenter: parent.verticalCenter
        width: 32
        height: 32
        radius: 16
        color: root.active ? Theme.palette.m3Primary : Theme.alpha(Theme.palette.m3OnSurface, 0.1)
        opacity: root.busy ? 0.6 : 1

        MaterialIcon {
            anchors.centerIn: parent
            text: root.glyph
            fill: root.active ? 1 : 0
            font.pointSize: Theme.font.size.normal
            color: root.active ? Theme.palette.m3OnPrimary : Theme.palette.m3OnSurface
        }

        SequentialAnimation on opacity {
            running: root.busy
            loops: Animation.Infinite

            Anim {
                to: 0.35
                duration: 500
            }
            Anim {
                to: 0.9
                duration: 500
            }
        }
    }

    MouseArea {
        id: area

        anchors.fill: parent
        hoverEnabled: true
        onClicked: root.clicked()
    }

    Column {
        anchors.left: badge.right
        anchors.leftMargin: 12
        anchors.right: buttons.left
        anchors.rightMargin: 8
        anchors.verticalCenter: parent.verticalCenter

        StyledText {
            width: parent.width
            text: root.name
            elide: Text.ElideRight
            font.weight: root.active ? Font.DemiBold : Font.Normal
        }

        StyledText {
            width: parent.width
            visible: text.length > 0
            text: root.note
            elide: Text.ElideRight
            font.pointSize: Theme.font.size.small
            color: Theme.palette.m3OnSurfaceVariant
        }
    }

    Row {
        id: buttons

        anchors.right: parent.right
        anchors.rightMargin: 8
        anchors.verticalCenter: parent.verticalCenter
        spacing: 6
    }
}
