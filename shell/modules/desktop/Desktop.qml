pragma ComponentBehavior: Bound

import QtQuick
import Quickshell
import Quickshell.Wayland
import qs.components
import qs.services
import Atrium

// Files on the desktop, as macOS and Windows show them: the desktop folder's
// contents in a grid from the top left, above the wallpaper and below
// windows. Apps that put shortcuts on the desktop (Steam) write .desktop
// files into this folder.
PanelWindow {
    id: desktop

    DesktopFiles {
        id: files
    }

    property var menu: null  // { x, y, path } of an open context menu; path "" for the desktop

    readonly property int cellWidth: 120
    readonly property int cellHeight: 108
    readonly property int margin: 18
    readonly property int rows: Math.max(1, Math.floor((height - margin * 2) / cellHeight))

    anchors {
        top: true
        bottom: true
        left: true
        right: true
    }
    color: "transparent"
    exclusiveZone: 0
    visible: Atrium.settings["desktop.icons"] ?? true
    WlrLayershell.layer: WlrLayer.Bottom
    WlrLayershell.namespace: "atrium-desktop"
    // Typing a new name needs the keyboard now, not after another click.
    WlrLayershell.keyboardFocus: files.renaming ? WlrKeyboardFocus.Exclusive : WlrKeyboardFocus.None

    // Clicks on the bare desktop: deselect, or its menu.
    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.LeftButton | Qt.RightButton
        onClicked: event => {
            Panels.open = "";
            files.click("", false, false);
            desktop.menu = event.button === Qt.RightButton ? { x: event.x, y: event.y, path: "" } : null;
        }
    }

    // Files dropped here from an app move into the desktop folder.
    DropArea {
        anchors.fill: parent
        keys: ["text/uri-list"]
        onDropped: drop => {
            if (files.moveIn(drop.urls))
                drop.accept(Qt.MoveAction);
        }
    }

    Repeater {
        model: files

        DesktopIcon {
            id: icon

            required property int index

            // Columns fill down from the top left.
            x: desktop.margin + Math.floor(index / desktop.rows) * desktop.cellWidth
            y: desktop.margin + (index % desktop.rows) * desktop.cellHeight

            selected: files.selection.includes(path)
            renaming: files.renaming === path

            onClicked: event => {
                files.click(path, event.modifiers & Qt.ControlModifier, event.button === Qt.RightButton);
                desktop.menu = event.button === Qt.RightButton ? { x: x + event.x, y: y + event.y, path: path } : null;
            }
            onDoubleClicked: files.open(path)
            onRenamed: name => files.rename(path, name)
        }
    }

    Menu {
        id: contextMenu

        visible: desktop.menu !== null
        x: Math.min(desktop.menu?.x ?? 0, desktop.width - width - 8)
        y: Math.min(desktop.menu?.y ?? 0, desktop.height - height - 8)
        title: {
            const p = desktop.menu?.path ?? "";
            if (!p)
                return "Desktop";
            const n = files.targets(p).length;
            return n > 1 ? `${n} items` : p.slice(p.lastIndexOf("/") + 1);
        }
        actions: {
            const p = desktop.menu?.path ?? "";
            if (!p)
                return [
                    { icon: "create_new_folder", text: "New Folder", run: () => files.newFolder() },
                    { icon: "terminal", text: "Open Terminal Here", run: () => files.terminalHere() },
                    { icon: "folder_open", text: "Open in Files", run: () => files.openFolder() }
                ];
            const single = files.targets(p).length === 1;
            return [
                { icon: "open_in_new", text: "Open", run: () => files.open(p) },
                ...(single ? [
                    { icon: "edit", text: "Rename", run: () => files.renaming = p },
                    { icon: "content_copy", text: "Copy Path", run: () => files.copyPath(p) }
                ] : []),
                "-",
                { icon: "delete", text: "Move to Trash", danger: true, run: () => files.trash(p) }
            ];
        }
        onPicked: desktop.menu = null
    }

    Connections {
        target: Atrium

        // Something else took focus: the menu is stale.
        function onFocusedWindowChanged(): void {
            // A window taking focus means a click elsewhere; the panel taking the
            // keyboard itself leaves no window focused and must not close it.
            if (!Atrium.focusedWindow)
                return;
            desktop.menu = null;
        }
    }
}
