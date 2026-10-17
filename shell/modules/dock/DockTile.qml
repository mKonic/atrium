pragma ComponentBehavior: Bound

import QtQuick
import Atrium.Shell
import shell.components
import shell.services
import Atrium

// A folder at the Dock's end (Downloads, the Trash): sized and magnified as
// the apps beside it, its name on hover, its own menu (menuRows, pick).
Item {
    id: root

    required property real iconSize
    required property real magnification
    property string name
    property string icon            // a theme icon name
    property var menuRows: []       // DockMenu's rows
    property bool menuOpen: false
    property bool dropOver: false
    readonly property bool isTile: true
    readonly property bool hovered: mouse.containsMouse && !mouse.pressed && !menuOpen

    signal activated
    signal menuRequested(Item item)
    signal picked(string key)
    function pick(key: string): void { picked(key); }

    // Something arrived (a finished download): one hop.
    function hop(): void {
        if (Theme.anim.on)
            arrival.restart();
    }

    readonly property real gap: 8
    implicitWidth: iconSize * magnification + gap
    implicitHeight: iconSize

    Behavior on implicitWidth {
        SmoothedAnimation {
            velocity: -1
            duration: Theme.anim.small
        }
    }

    Accessible.role: Accessible.Button
    Accessible.name: name
    Accessible.onPressAction: activated()

    Rectangle {
        anchors.centerIn: image
        width: image.width + 10
        height: image.height + 10
        radius: Theme.rounding.normal
        color: Theme.palette.tertiaryFill
        border.width: 2
        border.color: Theme.palette.accent
        opacity: root.dropOver ? 1 : 0
        visible: opacity > 0

        Behavior on opacity {
            Anim {
                duration: Theme.anim.small
            }
        }
    }

    IconImage {
        id: image

        property real offset: 0

        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: offset + (mouse.pressed ? -2 : 0)
        implicitSize: root.iconSize * root.magnification
        largestSize: root.iconSize * 1.55
        source: Shell.iconPath(root.icon, "folder")
        asynchronous: true
        smooth: true
        mipmap: true

        Behavior on implicitSize {
            SmoothedAnimation {
                velocity: -1
                duration: Theme.anim.small
            }
        }
    }

    SequentialAnimation {
        id: arrival

        Anim {
            target: image
            property: "offset"
            to: root.iconSize * 0.35
            duration: 260
            easing.bezierCurve: Theme.anim.emphasizedDecel
        }
        Anim {
            target: image
            property: "offset"
            to: 0
            duration: 300
            easing.type: Easing.OutBounce
        }
    }

    MouseArea {
        id: mouse

        anchors.fill: parent
        hoverEnabled: true
        acceptedButtons: Qt.LeftButton | Qt.RightButton
        onClicked: event => {
            if (event.button === Qt.RightButton)
                root.menuRequested(root);
            else
                root.activated();
        }
    }
}
