pragma ComponentBehavior: Bound

import QtQuick
import Atrium.Shell
import shell.components
import shell.services
import Atrium

// Files on the desktop, as Plasma's and Finder's desktops have them: the
// desktop folder's contents in a grid, above the wallpaper and below
// windows. Click, Ctrl/Shift+click or drag a rectangle to select; the keys
// a file manager has (Delete, F2, Enter, Ctrl+C/X/V/Z/A, arrows, typing a
// name); drag icons to move them, onto a folder or an app, or out to an
// app. Apps that put shortcuts on the desktop (Steam) write .desktop files
// into this folder. The first screen shows the files; every one has the menu.
PanelWindow {
    id: desktop

    // Whether this screen shows the files.
    property bool primary: true

    DesktopFiles {
        id: files

        area: Qt.size(desktop.width, desktop.height)
        cell: desktop.sizes.cell
        margin: 18

        onMenuRequested: path => {
            const at = files.spot(path);
            desktop.openMenu(at.x, at.y, path, false);
        }
        onDeleteRequested: paths => desktop.confirm = paths
        onRenamingChanged: if (!files.renaming) keys.forceActiveFocus()
    }

    // { x, y, path, page, shift } of an open context menu; path "" for the desktop.
    property var menu: null
    // Files asked to be deleted for good, waiting for a yes.
    property var confirm: null

    readonly property var sizes: ({
            small: { icon: 40, cell: Qt.size(100, 92) },
            medium: { icon: 54, cell: Qt.size(120, 108) },
            large: { icon: 72, cell: Qt.size(144, 130) }
        })[Atrium.settings["desktop.icon_size"] ?? "medium"] ?? { icon: 54, cell: Qt.size(120, 108) }

    function openMenu(x: real, y: real, path: string, shift: bool): void {
        Panels.open = "";
        desktop.confirm = null;
        desktop.menu = { x: x, y: y, path: path, page: "", shift: shift };
    }

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
    // A click on the desktop gives it the keyboard, as a window's would.
    WlrLayershell.keyboardFocus: WlrKeyboardFocus.OnDemand

    Item {
        id: keys

        anchors.fill: parent
        focus: true
        Keys.onPressed: event => {
            if (desktop.menu !== null && event.key === Qt.Key_Escape) {
                desktop.menu = null;
                event.accepted = true;
                return;
            }
            event.accepted = desktop.primary && files.key(event.key, event.modifiers, event.text);
        }
    }

    // The bare desktop: a press deselects, a drag selects what its
    // rectangle touches, a right click is the desktop's menu.
    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.LeftButton | Qt.RightButton
        onPressed: event => {
            keys.forceActiveFocus();
            Panels.open = "";
            desktop.menu = null;
            desktop.confirm = null;
            if (event.button === Qt.RightButton) {
                files.press("", event.modifiers, true);
                desktop.openMenu(event.x, event.y, "", false);
            } else {
                files.beginBand(event.x, event.y, event.modifiers);
            }
        }
        onPositionChanged: event => {
            if (pressedButtons & Qt.LeftButton)
                files.moveBand(event.x, event.y);
        }
        onReleased: files.endBand()
        onCanceled: files.endBand()
    }

    // Files dropped here: from an app they come in (Ctrl copies), on a
    // folder they go in, on an app they open, and ours move to the spot.
    DropArea {
        anchors.fill: parent
        keys: ["text/uri-list"]
        onPositionChanged: drag => files.dragOver(drag.x, drag.y, drag.urls)
        onExited: files.dragOver(-1, -1, [])
        onDropped: drop => {
            const copy = drop.proposedAction === Qt.CopyAction;
            if (files.drop(drop.urls, drop.x, drop.y, copy))
                drop.accept(copy ? Qt.CopyAction : Qt.MoveAction);
        }
    }

    Repeater {
        model: desktop.primary ? files : null

        DesktopIcon {
            required property real gridX
            required property real gridY

            x: gridX
            y: gridY
            width: files.cell.width
            height: files.cell.height
            iconSize: desktop.sizes.icon
            renaming: files.renaming === path
            dropTarget: files.dropTarget === path

            onPressed: (event, right) => {
                keys.forceActiveFocus();
                desktop.menu = null;
                desktop.confirm = null;
                files.press(path, event.modifiers, right);
                uris = files.dragUris(path);
                if (right)
                    desktop.openMenu(x + event.x, y + event.y, path, event.modifiers & Qt.ShiftModifier);
            }
            onReleased: (event, onName) => files.release(path, event.modifiers, onName)
            onDoubleClicked: files.activate(path)
            onRenamed: name => files.rename(path, name)
            onRenameDone: files.renaming = ""
        }
    }

    // The rectangle a drag on the bare desktop selects with.
    Rectangle {
        x: files.band.x
        y: files.band.y
        width: files.band.width
        height: files.band.height
        visible: width > 0 || height > 0
        radius: Theme.rounding.small / 2
        color: Theme.palette.accentFill
        border.width: 1
        border.color: Theme.palette.accent
    }

    Menu {
        id: contextMenu

        readonly property string path: desktop.menu?.path ?? ""
        readonly property var targets: path ? files.targets(path) : []
        readonly property bool single: targets.length === 1

        visible: desktop.menu !== null
        x: Math.min(desktop.menu?.x ?? 0, desktop.width - width - 8)
        y: Math.min(desktop.menu?.y ?? 0, desktop.height - height - 8)
        title: {
            const page = desktop.menu?.page ?? "";
            if (page === "sort")
                return qsTr("Arrange Icons");
            if (page === "new")
                return qsTr("New Document");
            if (page === "openwith")
                return qsTr("Open With");
            if (!path)
                return qsTr("Desktop");
            return targets.length > 1 ? qsTr("%1 items").arg(targets.length) : path.slice(path.lastIndexOf("/") + 1);
        }
        actions: {
            const page = desktop.menu?.page ?? "";
            const back = { icon: "arrow_back", text: qsTr("Back"), keep: true, run: () => desktop.menu = Object.assign({}, desktop.menu, { page: "" }) };
            const open = p => ({ keep: true, trailing: "chevron_right", run: () => desktop.menu = Object.assign({}, desktop.menu, { page: p }) });
            if (page === "sort") {
                const by = files.sortBy;
                const pick = (value, text) => ({ text: text, trailing: by === value ? "check" : "", run: () => files.sortBy = value });
                return [back, "-", pick("manual", qsTr("Where I Put Them")), pick("name", qsTr("Name")), pick("kind", qsTr("Kind")), pick("date", qsTr("Date Modified")), pick("size", qsTr("Size"))];
            }
            if (page === "new")
                return [back, "-", { icon: "description", text: qsTr("Text File"), run: () => files.newFile("") }, ...files.templates().map(t => ({ icon: "draft", text: t.label, run: () => files.newFile(t.path) }))];
            if (page === "openwith") {
                const apps = files.apps(path);
                return [back, "-", ...(apps.length ? apps.map(a => ({ text: a.label, run: () => files.openWith(path, a.value) })) : [{ text: qsTr("No apps for this kind"), enabled: false }])];
            }
            if (!path)
                return [
                    { icon: "create_new_folder", text: qsTr("New Folder"), hint: "Ctrl+Shift+N", run: () => files.newFolder() },
                    Object.assign({ icon: "note_add", text: qsTr("New Document") }, open("new")),
                    "-",
                    { icon: "content_paste", text: qsTr("Paste"), hint: "Ctrl+V", enabled: files.canPaste, run: () => files.paste() },
                    { icon: "undo", text: files.undoText ? qsTr("Undo %1").arg(files.undoText) : qsTr("Undo"), hint: "Ctrl+Z", enabled: files.undoText !== "", run: () => files.undo() },
                    "-",
                    Object.assign({ icon: "sort", text: qsTr("Arrange Icons") }, open("sort")),
                    { icon: "select_all", text: qsTr("Select All"), hint: "Ctrl+A", run: () => files.key(Qt.Key_A, Qt.ControlModifier, "") },
                    "-",
                    { icon: "terminal", text: qsTr("Open Terminal Here"), run: () => files.terminalHere() },
                    { icon: "folder_open", text: qsTr("Open in Files"), run: () => files.openFolder() },
                    { icon: "wallpaper", text: qsTr("Change Wallpaper…"), run: () => Atrium.action("shell", "settings:Appearance") }
                ];
            const name = path.slice(path.lastIndexOf("/") + 1);
            const isDir = files.isDir(path);
            return [
                { icon: "open_in_new", text: qsTr("Open"), hint: "Enter", run: () => files.open(path) },
                ...(single && !isDir ? [Object.assign({ icon: "apps", text: qsTr("Open With") }, open("openwith"))] : []),
                ...(single && files.isLink(path) ? [{ icon: "shortcut", text: qsTr("Show Original"), run: () => files.showOriginal(path) }] : []),
                "-",
                { icon: "content_cut", text: qsTr("Cut"), hint: "Ctrl+X", run: () => files.cut(path) },
                { icon: "content_copy", text: qsTr("Copy"), hint: "Ctrl+C", run: () => files.copy(path) },
                ...(single && isDir ? [{ icon: "content_paste", text: qsTr("Paste Into Folder"), enabled: files.canPaste, run: () => files.paste(path) }] : []),
                ...(single ? [
                    { icon: "edit", text: qsTr("Rename"), hint: "F2", run: () => files.renaming = path },
                    { icon: "link", text: qsTr("Copy Path"), run: () => files.copyPath(path) }
                ] : []),
                "-",
                desktop.menu?.shift
                    ? { icon: "delete_forever", text: qsTr("Delete Permanently…"), hint: "Shift+Del", danger: true, run: () => desktop.confirm = targets }
                    : { icon: "delete", text: qsTr("Move to Trash"), hint: "Del", danger: true, run: () => files.trash(path) }
            ];
        }
        onPicked: desktop.menu = null
    }

    // Deleting for good asks first: there's no Trash to get it back from.
    Menu {
        visible: desktop.confirm !== null
        x: (desktop.width - width) / 2
        y: (desktop.height - height) / 3
        title: desktop.confirm?.length > 1
            ? qsTr("Delete these %1 items for good? This can't be undone.").arg(desktop.confirm.length)
            : qsTr("Delete “%1” for good? This can't be undone.").arg((desktop.confirm?.[0] ?? "").slice((desktop.confirm?.[0] ?? "").lastIndexOf("/") + 1))
        actions: [
            { icon: "delete_forever", text: qsTr("Delete"), danger: true, run: () => files.deleteNow(desktop.confirm) },
            { icon: "close", text: qsTr("Cancel"), run: () => {} }
        ]
        onPicked: desktop.confirm = null
    }

    Connections {
        target: Atrium

        // A press anywhere else closes the menu (see Dock.qml).
        function onPointerPressed(layerNamespace: string): void {
            if (layerNamespace !== "atrium-desktop") {
                desktop.menu = null;
                desktop.confirm = null;
            }
        }
    }
}
