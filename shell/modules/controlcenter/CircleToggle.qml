import QtQuick
import shell.components
import shell.services

// A one-cell round button (Do Not Disturb, Night Light): white when on, as
// macOS 26's are. Its name shows on hover.
Module {
    id: root

    property string icon
    property string title
    property bool on
    signal toggled

    radius: width / 2
    color: on ? "white" : Theme.material.regular

    Behavior on color {
        CAnim {
            duration: Theme.anim.small
        }
    }

    MaterialIcon {
        anchors.centerIn: parent
        text: root.icon
        fill: root.on ? 1 : 0
        font.pointSize: Theme.font.size.larger + 2
        color: root.on ? "black" : Theme.palette.label
    }

    MouseArea {
        id: area

        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onClicked: root.toggled()
    }

    // Name on hover, under the button.
    Rectangle {
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.top: parent.bottom
        anchors.topMargin: 4
        z: 2
        width: label.implicitWidth + 14
        height: label.implicitHeight + 6
        radius: height / 2
        color: Theme.material.thick
        opacity: area.containsMouse ? 1 : 0
        visible: opacity > 0

        Behavior on opacity {
            Anim {
                duration: Theme.anim.small
            }
        }

        StyledText {
            id: label

            anchors.centerIn: parent
            text: root.title
            font.pointSize: Theme.font.size.small
        }
    }
}
