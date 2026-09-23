import QtQuick
import qs.services

// An on/off switch, as the header of an expanded Control Center module has.
Rectangle {
    id: root

    property bool checked: false
    signal toggled

    implicitWidth: 40
    implicitHeight: 24
    radius: height / 2
    color: checked ? Theme.palette.m3Primary : Theme.alpha(Theme.palette.m3OnSurface, 0.16)

    Behavior on color {
        CAnim {
            duration: Theme.anim.small
        }
    }

    Rectangle {
        x: root.checked ? root.width - width - 3 : 3
        anchors.verticalCenter: parent.verticalCenter
        width: root.height - 6
        height: width
        radius: width / 2
        color: root.checked ? Theme.palette.m3OnPrimary : Theme.palette.m3OnSurfaceVariant

        Behavior on x {
            Anim {
                duration: Theme.anim.small
                easing.bezierCurve: Theme.anim.emphasizedDecel
            }
        }
    }

    MouseArea {
        anchors.fill: parent
        cursorShape: Qt.PointingHandCursor
        onClicked: root.toggled()
    }
}
