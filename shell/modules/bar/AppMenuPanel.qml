import QtQuick
import Atrium.Shell
import shell.components
import shell.services
import Atrium

// An app menu, open under its title. Submenus open in its place. A click
// anywhere else closes it; moving along the menu bar's titles opens theirs.
PanelWindow {
    id: root

    property var spans: []      // the titles: [{ x, width, bottom, menu }]
    property int index: -1      // the one open
    property var entries: []    // the level shown
    property string heading: "" // the submenu's name, inside one

    readonly property string key: "appmenu:" + (screen?.name ?? "")

    function show(all: var, i: int): void {
        spans = all;
        index = i;
        entries = all[i].menu ? AppMenu.items(all[i].menu.id) : all[i].entries;
        heading = "";
        Panels.open = key;
    }

    function close(): void {
        if (Panels.open === key)
            Panels.open = "";
    }

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
    WlrLayershell.namespace: "atrium-app-menu"
    WlrLayershell.keyboardFocus: visible ? WlrKeyboardFocus.Exclusive : WlrKeyboardFocus.None

    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.AllButtons
        hoverEnabled: true
        // Along the titles: the one under the pointer opens; elsewhere a
        // click closes.
        onPositionChanged: mouse => {
            const s = root.spans.findIndex(t => mouse.x >= t.x && mouse.x < t.x + t.width && mouse.y < t.bottom);
            if (s >= 0 && s !== root.index)
                root.show(root.spans, s);
        }
        onPressed: mouse => {
            const s = root.spans.findIndex(t => mouse.x >= t.x && mouse.x < t.x + t.width && mouse.y < t.bottom);
            if (s < 0 || s === root.index)
                root.close();
        }
    }

    Menu {
        id: menu

        x: Math.max(8, Math.min(root.spans[root.index]?.x ?? 0, root.width - width - 8))
        y: (root.spans[root.index]?.bottom ?? 0) + 4
        focus: true
        title: root.heading
        Keys.onEscapePressed: root.close()
        Keys.onLeftPressed: if (root.index > 0) root.show(root.spans, root.index - 1)
        Keys.onRightPressed: if (root.index < root.spans.length - 1) root.show(root.spans, root.index + 1)
        actions: root.entries.map(e => e.separator ? "-" : {
            icon: e.checked ? "check" : "",
            trailing: e.submenu ? "chevron_right" : "",
            hint: e.shortcut,
            text: e.text,
            enabled: e.enabled,
            keep: e.submenu,
            run: () => {
                if (!e.submenu)
                    return AppMenu.trigger(e.id);
                root.heading = e.text;
                root.entries = e.children.length > 0 ? e.children : AppMenu.items(e.id);
            }
        })
        onPicked: root.close()
    }
}
