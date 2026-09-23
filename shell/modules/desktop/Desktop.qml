pragma ComponentBehavior: Bound

import QtQuick
import Qt.labs.folderlistmodel
import Quickshell
import Quickshell.Io
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

    readonly property string override: Quickshell.env("ATRIUM_DESKTOP_DIR") ?? ""
    property string folder: override || `${Quickshell.env("HOME")}/Desktop`
    property var selection: []
    property string renaming: ""
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
    WlrLayershell.keyboardFocus: renaming ? WlrKeyboardFocus.Exclusive : WlrKeyboardFocus.None

    // The XDG desktop folder, which need not be ~/Desktop.
    Process {
        running: !desktop.override
        command: ["xdg-user-dir", "DESKTOP"]
        stdout: StdioCollector {
            onStreamFinished: {
                const dir = text.trim();
                if (dir && dir !== Quickshell.env("HOME"))
                    desktop.folder = dir;
                // It has to exist to be watched, and for apps to put shortcuts in.
                desktop.run(["mkdir", "-p", desktop.folder]);
            }
        }
    }

    FolderListModel {
        id: files

        folder: `file://${desktop.folder}`
        showDirsFirst: true
        showHidden: false
        sortField: FolderListModel.Name
        sortCaseSensitive: false
    }

    function run(args: var): void {
        Quickshell.execDetached(args);
    }

    function quote(s: string): string {
        return `'${s.replace(/'/g, "'\\''")}'`;
    }

    function open(path: string): void {
        if (path.endsWith(".desktop"))
            run(["gio", "launch", path]);
        else
            run(["xdg-open", path]);
    }

    function newFolder(): void {
        let name = "New Folder";
        for (let n = 2; files.indexOf(`file://${folder}/${name}`) >= 0; n++)
            name = `New Folder ${n}`;
        run(["mkdir", "-p", `${folder}/${name}`]);
        renameLater.name = name;
        renameLater.restart();
    }

    function rename(path: string, name: string): void {
        renaming = "";
        const dir = path.slice(0, path.lastIndexOf("/"));
        if (name && !name.includes("/") && `${dir}/${name}` !== path)
            run(["mv", "-n", "--", path, `${dir}/${name}`]);
    }

    function terminalHere(): void {
        const term = Atrium.setting("shortcuts.terminal", "ghostty");
        run(["sh", "-c", `cd ${quote(folder)} && exec ${term}`]);
    }

    // A new folder appears a moment after mkdir; then it can be renamed.
    Timer {
        id: renameLater

        property string name

        interval: 300
        onTriggered: {
            desktop.selection = [`${desktop.folder}/${name}`];
            desktop.renaming = `${desktop.folder}/${name}`;
        }
    }

    // Clicks on the bare desktop: deselect, or its menu.
    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.LeftButton | Qt.RightButton
        onClicked: event => {
            desktop.selection = [];
            desktop.renaming = "";
            desktop.menu = event.button === Qt.RightButton ? { x: event.x, y: event.y, path: "" } : null;
        }
    }

    // Files dropped here from an app move into the desktop folder.
    DropArea {
        anchors.fill: parent
        keys: ["text/uri-list"]
        onDropped: drop => {
            const paths = drop.urls.map(u => decodeURIComponent(String(u).replace(/^file:\/\//, "")))
                .filter(p => p && !p.startsWith(`${desktop.folder}/`));
            if (paths.length === 0)
                return;
            desktop.run(["mv", "-n", "--", ...paths, desktop.folder]);
            drop.accept(Qt.MoveAction);
        }
    }

    Repeater {
        model: files

        DesktopIcon {
            required property int index
            required property string filePath
            required property string fileName
            required property string fileSuffix
            required property bool fileIsDir

            // Columns fill down from the top left.
            x: desktop.margin + Math.floor(index / desktop.rows) * desktop.cellWidth
            y: desktop.margin + (index % desktop.rows) * desktop.cellHeight

            path: filePath
            name: fileName
            suffix: fileSuffix
            isDir: fileIsDir
            selected: desktop.selection.includes(filePath)
            renaming: desktop.renaming === filePath

            onClicked: event => {
                desktop.renaming = "";
                if (event.modifiers & Qt.ControlModifier) {
                    const s = desktop.selection.slice();
                    const i = s.indexOf(filePath);
                    i >= 0 ? s.splice(i, 1) : s.push(filePath);
                    desktop.selection = s;
                } else if (!desktop.selection.includes(filePath) || event.button === Qt.LeftButton) {
                    desktop.selection = [filePath];
                }
                desktop.menu = event.button === Qt.RightButton
                    ? { x: x + event.x, y: y + event.y, path: filePath } : null;
            }
            onDoubleClicked: desktop.open(filePath)
            onRenamed: name => desktop.rename(filePath, name)
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
            const n = desktop.selection.length;
            return n > 1 ? `${n} items` : p.slice(p.lastIndexOf("/") + 1);
        }
        actions: {
            const p = desktop.menu?.path ?? "";
            if (!p)
                return [
                    { icon: "create_new_folder", text: "New Folder", run: () => desktop.newFolder() },
                    { icon: "terminal", text: "Open Terminal Here", run: () => desktop.terminalHere() },
                    { icon: "folder_open", text: "Open in Files", run: () => desktop.run(["xdg-open", desktop.folder]) }
                ];
            const items = desktop.selection.length > 1 ? desktop.selection : [p];
            const list = [
                { icon: "open_in_new", text: "Open", run: () => items.forEach(f => desktop.open(f)) }
            ];
            if (items.length === 1) {
                list.push({ icon: "edit", text: "Rename", run: () => desktop.renaming = p });
                list.push({ icon: "content_copy", text: "Copy Path", run: () => Quickshell.clipboardText = p });
            }
            list.push("-");
            list.push({ icon: "delete", text: "Move to Trash", danger: true, run: () => desktop.run(["gio", "trash", "--", ...items]) });
            return list;
        }
        onPicked: desktop.menu = null
    }

    Connections {
        target: Atrium

        // Something else took focus: the menu is stale.
        function onFocusedWindowChanged(): void {
            desktop.menu = null;
        }
    }
}
