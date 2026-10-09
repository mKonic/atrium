pragma ComponentBehavior: Bound

import QtQuick
import Atrium.Shell
import Atrium
import shell.components
import shell.services

// The privacy dots, as a Mac's: shown only while an app shares the screen
// (purple), uses a camera (green) or a microphone (orange). A click says which.
Pill {
    id: root

    required property var bar  // the panel window, for placing the menu

    visible: Privacy.active
    implicitWidth: dots.implicitWidth + Theme.padding.normal * 2

    Row {
        id: dots

        anchors.centerIn: parent
        spacing: 4

        Repeater {
            model: [
                { on: Privacy.screen, color: Theme.palette.purple },
                { on: Privacy.camera, color: Theme.palette.green },
                { on: Privacy.microphone, color: Theme.palette.orange }
            ]

            Rectangle {
                required property var modelData

                visible: modelData.on
                width: 8
                height: 8
                radius: 4
                color: modelData.color
            }
        }
    }

    MouseArea {
        anchors.fill: parent
        onClicked: {
            const p = mapToItem(null, 0, height + 6);
            menu.at = Qt.point(p.x, p.y);
            Panels.open = menu.key;
        }
    }

    PanelWindow {
        id: menu

        property point at
        readonly property string key: "privacy:" + (screen?.name ?? "")

        screen: root.bar.screen
        visible: Panels.open === key
        onVisibleChanged: if (visible) list.forceActiveFocus()
        anchors {
            top: true
            bottom: true
            left: true
            right: true
        }
        exclusionMode: ExclusionMode.Ignore
        color: "transparent"
        WlrLayershell.layer: WlrLayer.Overlay
        WlrLayershell.namespace: "atrium-privacy-menu"
        WlrLayershell.keyboardFocus: visible ? WlrKeyboardFocus.Exclusive : WlrKeyboardFocus.None

        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.AllButtons
            onPressed: Panels.open = ""
        }

        Menu {
            id: list

            x: Math.max(8, Math.min(menu.at.x, menu.width - width - 8))
            y: menu.at.y
            focus: true
            Keys.onEscapePressed: Panels.open = ""
            actions: Privacy.uses
            onPicked: Panels.open = ""
        }
    }
}
