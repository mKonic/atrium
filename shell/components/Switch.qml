import QtQuick
import shell.services

// An on/off switch, as macOS 27 has it: held, its knob turns to glass over
// the track (and slides when let go).
Item {
    id: root

    property bool checked: false
    // What it turns on and off, for screen readers (its label is the row's).
    property string name
    signal toggled

    Accessible.role: Accessible.CheckBox
    Accessible.name: name
    Accessible.checkable: true
    Accessible.checked: checked
    Accessible.onToggleAction: toggled()
    Accessible.onPressAction: toggled()

    // The kit's form switch (Mini): a long track, its knob a capsule.
    implicitWidth: 48
    implicitHeight: 22

    Rectangle {
        id: track

        anchors.fill: parent
        radius: height / 2
        color: root.checked ? Theme.palette.accent : Theme.palette.fill

        Behavior on color {
            CAnim {
                duration: Theme.anim.small
            }
        }
    }

    // Beside the track, not in it, so it can be a lens over it.
    Thumb {
        x: root.checked ? root.width - width - 2 : 2
        anchors.verticalCenter: parent.verticalCenter
        width: Math.round(root.width * 0.6)
        height: root.height - 4
        pressed: area.pressed
        backdrop: track

        Behavior on x {
            Anim {
                duration: Theme.anim.small
                easing.bezierCurve: Theme.anim.emphasizedDecel
            }
        }
    }

    MouseArea {
        id: area

        anchors.fill: parent
        cursorShape: Qt.PointingHandCursor
        onClicked: root.toggled()
    }
}
