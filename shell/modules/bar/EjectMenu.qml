pragma ComponentBehavior: Bound

import QtQuick
import Atrium.Shell
import Atrium
import shell.components
import shell.services

// While a USB drive or card is mounted: an eject button in the menu bar,
// listing them to open or eject.
Pill {
    id: root

    required property var bar  // the panel window, for placing the menu

    visible: Disks.ejectable.length > 0
    implicitWidth: implicitHeight

    MaterialIcon {
        anchors.centerIn: parent
        text: "eject"
        font.pointSize: Theme.font.size.normal
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
        readonly property string key: "eject:" + (screen?.name ?? "")

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
        WlrLayershell.namespace: "atrium-eject-menu"
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
            width: 240
            focus: true
            Keys.onEscapePressed: Panels.open = ""
            actions: Disks.ejectable.map(d => ({
                icon: "eject",
                text: `Eject ${d.name}`,
                run: () => Disks.eject(d.path)
            })).concat(["-"]).concat(Disks.ejectable.map(d => ({
                icon: "folder_open",
                text: `Open ${d.name}`,
                run: () => Disks.open(d.path)
            })))
            onPicked: Panels.open = ""
        }
    }
}
