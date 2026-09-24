import QtQuick
import Atrium.Shell
import shell.components
import shell.services

// A tray icon's menu, in the shell's own style, under the icon. Submenus
// open in its place. A click anywhere else closes it.
PanelWindow {
    id: root

    property var item: null     // the SystemTrayItem
    property var entries: []    // the level shown
    property string heading: ""  // the submenu's name, inside one
    property point at

    function open(tray: var, x: int, y: int): void {
        item = tray;
        entries = tray.menu();
        heading = "";
        at = Qt.point(x, y);
        if (entries.length > 0)
            Panels.open = key;
    }

    readonly property string key: "tray:" + (screen?.name ?? "")

    visible: Panels.open === key
    onVisibleChanged: if (visible) menu.forceActiveFocus()
    anchors {
        top: true
        bottom: true
        left: true
        right: true
    }
    exclusionMode: ExclusionMode.Ignore
    color: "transparent"
    WlrLayershell.layer: WlrLayer.Overlay
    WlrLayershell.namespace: "atrium-tray-menu"
    WlrLayershell.keyboardFocus: visible ? WlrKeyboardFocus.Exclusive : WlrKeyboardFocus.None

    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.AllButtons
        onPressed: Panels.open = ""
    }

    Menu {
        id: menu

        // Under the icon, kept on the screen.
        x: Math.max(8, Math.min(root.at.x, root.width - width - 8))
        y: root.at.y
        width: 240
        focus: true
        title: root.heading || (root.item?.title ?? "")
        Keys.onEscapePressed: Panels.open = ""
        actions: root.entries.map(e => e.separator ? "-" : {
            icon: e.checked ? "check" : "",
            trailing: e.children.length > 0 ? "chevron_right" : "",
            text: e.text,
            enabled: e.enabled,
            keep: e.children.length > 0,
            run: () => {
                if (e.children.length === 0)
                    return root.item.trigger(e.id);
                root.heading = e.text;
                root.entries = e.children;
            }
        })
        onPicked: Panels.open = ""
    }
}
