pragma ComponentBehavior: Bound

import QtQuick
import Atrium.Shell
import Atrium
import shell.components
import shell.services

// The keyboard layout in use, as macOS's input menu: its short name in the
// bar while there's more than one; a click lists them to pick from.
Pill {
    id: root

    required property var bar  // the panel window, for placing the menu

    readonly property var layouts: Atrium.keyboard.layouts ?? []
    readonly property int active: Atrium.keyboard.active ?? 0

    visible: layouts.length > 1
    implicitWidth: code.implicitWidth + Theme.padding.normal * 2

    StyledText {
        id: code

        anchors.centerIn: parent
        text: (root.layouts[root.active]?.code ?? "").toUpperCase()
        font.pointSize: Theme.font.size.small
        font.weight: Font.DemiBold
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
        readonly property string key: "input:" + (screen?.name ?? "")

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
        WlrLayershell.namespace: "atrium-input-menu"
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
            actions: root.layouts.map((l, i) => ({
                icon: i === root.active ? "check" : "",
                text: l.name,
                run: () => Atrium.setKeyboardLayout(i)
            })).concat(["-", {
                icon: "",
                text: "Keyboard Settings…",
                run: () => Atrium.action("shell", "settings:Keyboard")
            }])
            onPicked: Panels.open = ""
        }
    }
}
