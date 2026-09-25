import QtQuick
import shell.components
import shell.services

// A two-cell capsule (Wi-Fi, Bluetooth): the icon's disc switches it, as on
// a Mac, and the rest opens its list. On, the disc is white with the icon in
// the accent.
Module {
    id: root

    property string icon
    property string title
    property string subtitle
    property bool on
    property bool expandable
    signal toggled
    signal expand

    radius: height / 2

    Rectangle {
        id: disc

        x: (root.height - width) / 2
        anchors.verticalCenter: parent.verticalCenter
        width: root.height - 24
        height: width
        radius: width / 2
        color: root.on ? "white" : Theme.palette.tertiaryFill

        Behavior on color {
            CAnim {
                duration: Theme.anim.small
            }
        }

        MaterialIcon {
            anchors.centerIn: parent
            text: root.icon
            fill: 1
            font.pointSize: Theme.font.size.larger
            color: root.on ? Theme.palette.accent : Theme.palette.label
        }

        MouseArea {
            anchors.fill: parent
            cursorShape: Qt.PointingHandCursor
            onClicked: root.toggled()
        }
    }

    Column {
        anchors.left: disc.right
        anchors.leftMargin: 8
        anchors.right: parent.right
        anchors.rightMargin: 12
        anchors.verticalCenter: parent.verticalCenter

        StyledText {
            width: parent.width
            text: root.title
            elide: Text.ElideRight
            font.weight: Font.DemiBold
            font.pointSize: Theme.font.size.small
        }

        // Two lines if it needs them, as "Contacts Only" takes on a Mac.
        StyledText {
            width: parent.width
            text: root.subtitle
            wrapMode: Text.Wrap
            maximumLineCount: 2
            elide: Text.ElideRight
            lineHeight: 0.9
            font.pointSize: Theme.font.size.small
            color: Theme.palette.secondaryLabel
        }
    }

    MouseArea {
        anchors.fill: parent
        anchors.leftMargin: disc.x + disc.width
        cursorShape: root.expandable ? Qt.PointingHandCursor : Qt.ArrowCursor
        onClicked: root.expandable ? root.expand() : root.toggled()
    }
}
