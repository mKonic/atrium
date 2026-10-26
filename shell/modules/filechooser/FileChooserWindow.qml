pragma ComponentBehavior: Bound

import QtQuick
import Atrium.Shell
import shell.components
import shell.modules.settings
import shell.services
import Atrium

// Opening or saving for an app: GTK 3's file chooser (what the GTK portal
// showed before), in atrium's look. Places on the left; the path bar (or a
// typed location, or a search) on top; the list; the app's file types and
// choices along the bottom. Behaviour, keys and wording follow
// gtkfilechooserwidget.c; the rules live in ChooserModel and FileChooser.
FloatingWindow {
    id: root

    readonly property string action: FileChooser.action  // open, save, folder
    readonly property bool saving: action === "save"
    property bool locating: false  // the location field instead of the path bar
    // Opened from the path bar: the folder's path, picked to type over. Left
    // as it was, clicking elsewhere turns it back into the path bar.
    property string locationShown: ""
    property string sidebarCurrent: model.mode === "recent" ? "recent" : otherLocations ? "other" : model.folder
    property bool otherLocations: false

    // Remembered view (GTK's org.gtk.Settings.FileChooser).
    function setting(key: string, fallback: var): var {
        return Atrium.settings["filechooser." + key] ?? fallback;
    }
    function remember(key: string, value: var): void {
        Atrium.setSetting("filechooser." + key, value);
    }

    // --- answering ---------------------------------------------------------

    // The dialog's default button, or Return: GTK's should_respond, by
    // which part has the focus.
    function accept(): void {
        if (prompt.open || otherLocations)
            return;
        // Recent and search answer with what's picked, never where the list is.
        if (model.mode !== "browse" && !saving && model.selection.length === 0)
            return;
        if (saving || (locating && locationField.text !== ""))
            applyText(FileChooser.acceptText(saving ? nameField.text : locationField.text, model.folder));
        else
            applyList(FileChooser.acceptList(model.selection, model.folder));
    }

    function applyList(next: var): void {
        if (next.enter)
            go(next.enter);
        else if (next.entry)
            applyText(FileChooser.acceptText(nameField.text, model.folder));
    }

    function applyText(next: var, typed: string): void {
        if (next.enter) {
            go(next.enter);
            if (next.clear && !saving && locationShown !== "")
                stopLocation();  // typed from the path bar: back to it
            else if (next.clear && !saving)
                locationField.setText("");
            else if (next.clear)
                nameField.text = nameField.text.slice(nameField.text.lastIndexOf("/") + 1);
        } else if (next.error) {
            prompt.ask({ title: next.error, detail: next.detail ?? "", cancel: false });
        } else if (next.confirm) {
            const text = saving ? nameField.text : locationField.text;
            prompt.ask({
                title: next.confirm,
                detail: next.detail,
                accept: qsTr("Replace"),
                run: () => applyText(FileChooser.acceptText(text, model.folder, true))
            });
        } else if (next.focus) {
            nameField.focusField();
        } else if (next.list) {
            applyList(FileChooser.acceptList(model.selection, model.folder));
        }
    }

    // A double click or Return on a row (later: the row goes away when the folder changes).
    function activate(index: int): void {
        Qt.callLater(() => {
            const r = model.activate(index);
            if (r.enter) {
                otherLocations = false;
                list.focusList();
            } else if (r.answer) {
                if (saving)
                    applyText(FileChooser.acceptText(model.row(index).name, model.folder));
                else
                    applyList(FileChooser.acceptList(model.selection, model.folder));
            }
        });
    }

    function go(path: string): void {
        otherLocations = false;
        model.folder = path;
        list.focusList();
    }

    function open(location: string): void {
        if (location === "recent") {
            otherLocations = false;
            model.showRecent();
        } else if (location === "other") {
            otherLocations = true;
        } else if (location !== "") {
            go(location);
        }
    }

    // --- the header's modes --------------------------------------------------

    function startLocation(text: string): void {
        if (model.mode === "search")
            model.stopSearch();
        if (saving) {
            nameField.focusField();
            if (text !== "")
                nameField.setText(text);
            return;
        }
        locating = true;
        locationShown = "";
        locationField.setText(text);
        locationField.focusField();
    }

    // The path bar clicked beside its buttons: the folder's path, ready to edit.
    function editLocation(): void {
        if (saving || model.mode !== "browse")
            return;
        const path = model.folder === "/" ? "/" : model.folder + "/";
        startLocation(path);
        locationShown = path;
        locationField.select(0, path.length);
    }

    function stopLocation(): void {
        locating = false;
        locationShown = "";
        list.focusList();
    }

    function toggleSearch(): void {
        if (model.mode === "search") {
            model.stopSearch();
            list.focusList();
        } else {
            locating = false;
            otherLocations = false;
            model.startSearch();
            searchField.focusField();
        }
    }

    // Ctrl+V: a path or URI pasted is picked, else typed as a location.
    function paste(): void {
        clip.text = "";
        clip.paste();
        const text = clip.text.trim();
        if (text === "")
            return;
        const r = model.resolve(text);
        if (r.exists && !r.isDir)
            model.selectPath(r.path);
        else
            startLocation(text);
    }

    // --- view settings -------------------------------------------------------

    function toggle(key: string, value: bool): void {
        remember(key, value);
    }

    title: FileChooser.title
    color: Theme.palette.windowBackground
    implicitWidth: setting("width", 0) > 0 ? setting("width", 0) : 900
    implicitHeight: setting("height", 0) > 0 ? setting("height", 0) : 600
    minimumSize: Qt.size(640, 420)
    onVisibleChanged: if (!visible) FileChooser.cancel()
    onWidthChanged: saveSize.restart()
    onHeightChanged: saveSize.restart()

    Timer {
        id: saveSize

        interval: 600
        onTriggered: {
            root.remember("width", Math.round(root.width));
            root.remember("height", Math.round(root.height));
        }
    }

    ChooserModel {
        id: model

        action: root.action
        multiple: FileChooser.multiple
        patterns: FileChooser.patterns
        showHidden: root.setting("show_hidden", false)
        foldersFirst: root.setting("folders_first", false)
        sortColumn: root.setting("sort_column", "name")
        ascending: root.setting("sort_ascending", true)
        showTime: root.setting("show_time", false)
        clock24: Atrium.settings["clock.24_hour"] ?? true
        onShowHiddenChanged: if (showHidden !== root.setting("show_hidden", false)) root.remember("show_hidden", showHidden)
        onFilterInTheWay: FileChooser.filter = -1
    }

    ChooserPlaces {
        id: places

        // Recent is offered when opening or choosing a folder (GTK hides it when saving).
        showRecent: !root.saving
    }

    Component.onCompleted: {
        // Where it starts: where the app said, else Recent (GTK's startup
        // mode), else home. A file to save over is picked in its folder.
        model.folder = FileChooser.folder;
        if (!FileChooser.startsSomewhere && !saving && setting("startup", "recent") === "recent")
            model.showRecent();
        if (FileChooser.file !== "")
            model.selectPath(FileChooser.file);
        if (saving) {
            nameField.setText(FileChooser.name);
            nameField.focusField();
            // The name without its suffix, to type over.
            const dot = FileChooser.name.lastIndexOf(".");
            nameField.select(0, dot > 0 ? dot : FileChooser.name.length);
        } else {
            list.focusList();
        }
    }

    // Clipboard text, through a field nobody sees.
    TextInput {
        id: clip

        visible: false
    }

    // --- keys (GTK's bindings) ---------------------------------------------

    Shortcut { sequence: "Ctrl+L"; onActivated: root.locating && !root.saving ? root.stopLocation() : root.startLocation("") }
    Shortcut { sequence: "Ctrl+V"; enabled: !root.locating && !nameField.editing && !searchField.editing; onActivated: root.paste() }
    Shortcut { sequences: ["Alt+Up"]; onActivated: model.up() }
    Shortcut { sequences: ["Alt+Down"]; onActivated: model.down() }
    Shortcut { sequence: "Alt+Home"; onActivated: root.go(model.home) }
    Shortcut { sequence: "Alt+D"; onActivated: root.open(places.places.find(p => p.name === "Desktop")?.path ?? "") }
    Shortcut { sequence: "Ctrl+H"; onActivated: root.remember("show_hidden", !model.showHidden) }
    Shortcut { sequences: ["Alt+S", "Ctrl+F"]; onActivated: root.toggleSearch() }
    Shortcut { sequence: "Alt+R"; enabled: !root.saving; onActivated: root.open("recent") }
    Shortcut { sequence: "Alt+P"; onActivated: sidebar.focusList() }
    Repeater {
        model: 10

        Item {
            required property int index

            Shortcut {
                sequence: "Alt+" + ((index + 1) % 10)
                onActivated: root.open(places.nthBookmark(index))
            }
        }
    }

    // Escape steps back: a question, a menu, the location field, a search; then cancels.
    Shortcut {
        sequence: "Escape"
        enabled: !locationField.editing && !nameField.editing
        onActivated: {
            if (prompt.open)
                prompt.close();
            else if (menu.shown)
                menu.shown = false;
            else if (model.mode === "search")
                root.toggleSearch();
            else if (root.locating)
                root.stopLocation();
            else
                FileChooser.cancel();
        }
    }

    Item {
        id: page

        anchors.fill: parent
        anchors.margins: 14

        // --- name to save under --------------------------------------------
        Row {
            id: saveRow

            visible: root.saving
            height: visible ? 32 : 0
            spacing: 10
            anchors.horizontalCenter: parent.horizontalCenter
            z: 3

            StyledText {
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("Name:")
            }

            PathField {
                id: nameField

                width: 360
                model: model
                inlineCompletion: false
                onAccepted: root.accept()
                onEscaped: FileChooser.cancel()
                // Typing a folder path here moves the list along with it.
                onTextChanged: {
                    const slash = text.lastIndexOf("/");
                    if (slash >= 0) {
                        const r = model.resolve(text);
                        if (r.dir !== "" && r.dir !== model.folder && model.resolve(r.dir + "/").isDir)
                            model.folder = r.dir;
                    }
                }
            }
        }

        // --- where -----------------------------------------------------------
        Item {
            id: header

            anchors.top: saveRow.bottom
            anchors.topMargin: saveRow.visible ? 12 : 0
            width: parent.width
            height: 32
            z: 2

            TrailBar {
                visible: !root.locating && model.mode === "browse" && !root.otherLocations
                anchors.left: parent.left
                anchors.right: tools.left
                anchors.rightMargin: 8
                height: parent.height
                model: model
                onEditRequested: root.editLocation()
            }

            StyledText {
                visible: model.mode === "recent" || root.otherLocations
                anchors.verticalCenter: parent.verticalCenter
                text: root.otherLocations ? qsTr("Other Locations") : qsTr("Recent")
                font.weight: Font.DemiBold
            }

            PathField {
                id: locationField

                visible: root.locating && model.mode === "browse"
                anchors.left: parent.left
                anchors.right: tools.left
                anchors.rightMargin: 8
                height: parent.height
                model: model
                placeholder: qsTr("Location")
                inlineCompletion: true
                onAccepted: root.accept()
                onEscaped: root.stopLocation()
                onEditingChanged: {
                    if (!editing && root.locating && root.locationShown !== "" && text === root.locationShown)
                        root.locating = false;
                }
            }

            Field {
                id: searchField

                readonly property bool editing: activeFocus
                visible: model.mode === "search"
                anchors.left: parent.left
                anchors.right: tools.left
                anchors.rightMargin: 8
                height: parent.height
                placeholder: qsTr("Search in %1").arg(model.folder === model.home ? qsTr("Home") : model.folder.slice(model.folder.lastIndexOf("/") + 1) || "/")
                onTextChanged: model.query = text
                onAccepted: list.focusList()
                Keys.onEscapePressed: root.toggleSearch()
                Keys.onDownPressed: list.focusList()
            }

            Row {
                id: tools

                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                spacing: 6

                // GTK's spinner, while a search is still walking.
                MaterialIcon {
                    visible: model.searching
                    anchors.verticalCenter: parent.verticalCenter
                    text: "progress_activity"
                    color: Theme.palette.secondaryLabel
                    RotationAnimation on rotation { running: model.searching; from: 0; to: 360; duration: 900; loops: Animation.Infinite }
                }

                ToolButton {
                    // Create Folder: not when opening (GTK), nor in Recent.
                    visible: root.action !== "open" && model.mode === "browse" && !root.otherLocations
                    icon: "create_new_folder"
                    name: qsTr("Create Folder")
                    onClicked: prompt.ask({
                        title: qsTr("Folder Name"),
                        field: true,
                        text: "",
                        accept: qsTr("Create"),
                        check: t => FileChooser.checkName(model.folder, t, true),
                        run: t => {
                            const r = FileChooser.makeFolder(model.folder, t);
                            if (r.path)
                                root.go(r.path);
                            else
                                prompt.ask({ title: qsTr("The folder could not be created"), detail: r.error, cancel: false });
                        }
                    })
                }

                ToolButton {
                    icon: "search"
                    name: qsTr("Search")
                    checked: model.mode === "search"
                    onClicked: root.toggleSearch()
                }
            }
        }

        // --- places, files, preview ------------------------------------------
        Item {
            id: middle

            anchors.top: header.bottom
            anchors.topMargin: 10
            anchors.bottom: footer.top
            anchors.bottomMargin: 10
            width: parent.width

            PlacesSidebar {
                id: sidebar

                width: Math.max(100, Math.min(400, root.setting("sidebar_width", 148) + split.drag))
                height: parent.height
                places: places
                current: root.sidebarCurrent
                onOpen: location => root.open(location)
                onMenuAt: (row, x, y) => menu.showFor("place", row, sidebar.mapToItem(page, x, y))
                onError: text => prompt.ask({ title: qsTr("Unable to mount"), detail: text, cancel: false })
            }

            // The sidebar's edge drags wider or narrower.
            MouseArea {
                id: split

                property real drag: 0
                property real startX

                x: sidebar.width + 2
                width: 8
                height: parent.height
                cursorShape: Qt.SplitHCursor
                onPressed: mouse => startX = mouse.x
                onPositionChanged: mouse => drag += mouse.x - startX
                onReleased: {
                    root.remember("sidebar_width", Math.round(sidebar.width));
                    drag = 0;
                }
            }

            FileList {
                id: list

                visible: !root.otherLocations
                anchors.left: sidebar.right
                anchors.leftMargin: 12
                anchors.right: preview.visible ? preview.left : parent.right
                anchors.rightMargin: preview.visible ? 12 : 0
                height: parent.height
                model: model
                showSize: root.setting("show_size", true)
                showType: root.setting("show_type", true)
                onActivated: index => root.activate(index)
                onAcceptRequested: root.accept()
                onMenuAt: (x, y) => menu.showFor("list", null, list.mapToItem(page, x, y))
                onSortBy: column => {
                    if (column === model.sortColumn)
                        root.remember("sort_ascending", !model.ascending);
                    else {
                        root.remember("sort_column", column);
                        root.remember("sort_ascending", true);
                    }
                }
                onToSidebar: sidebar.focusList()
                onDropped: paths => {
                    // One folder: gone into (opening or saving); else what's dropped is picked.
                    if (paths.length === 1 && model.resolve(paths[0]).isDir && root.action !== "folder")
                        root.go(paths[0]);
                    else
                        for (const p of paths)
                            model.selectPath(p);
                }

                Keys.onPressed: event => {
                    const t = event.text;
                    if (t === "" || event.modifiers & (Qt.ControlModifier | Qt.AltModifier) || t.charCodeAt(0) < 32)
                        return;
                    if ((t === "/" || t === "~" || t === ".") && !root.saving) {
                        root.startLocation(t === "~" ? "~" : t);
                    } else if (model.mode !== "search") {
                        root.toggleSearch();
                        searchField.text = t;
                    } else {
                        return;
                    }
                    event.accepted = true;
                }
            }

            OtherLocations {
                visible: root.otherLocations
                anchors.left: sidebar.right
                anchors.leftMargin: 12
                anchors.right: parent.right
                height: parent.height
                places: places
                onOpen: path => root.go(path)
            }

            // The GTK portal's preview: a picture picked, shown at 128 px.
            Item {
                id: preview

                readonly property string file: model.selection.length === 1 ? model.selection[0] : ""
                readonly property bool picture: file !== "" && /\.(png|jpe?g|gif|webp|bmp|svg|avif|tiff?|ico|heic)$/i.test(file)

                visible: picture && !root.otherLocations && previewImage.status !== Image.Error
                anchors.right: parent.right
                width: 148
                height: parent.height

                Image {
                    id: previewImage

                    anchors.horizontalCenter: parent.horizontalCenter
                    y: 10
                    width: 128
                    height: 128
                    source: preview.picture ? "file://" + preview.file : ""
                    sourceSize: Qt.size(256, 256)
                    fillMode: Image.PreserveAspectFit
                    asynchronous: true
                    autoTransform: true
                }
            }
        }

        // --- the app's types and choices, and the answer -----------------------
        Item {
            id: footer

            anchors.bottom: parent.bottom
            width: parent.width
            height: Math.max(buttons.height, options.height)

            Flow {
                id: options

                anchors.left: parent.left
                anchors.right: filterBox.left
                anchors.rightMargin: 16
                anchors.verticalCenter: parent.verticalCenter
                spacing: 12

                Repeater {
                    model: FileChooser.choices

                    Row {
                        id: choice

                        required property var modelData
                        readonly property bool toggle: modelData.options.length === 0

                        spacing: 8

                        Switch {
                            name: choice.modelData.label ?? ""
                            visible: choice.toggle
                            anchors.verticalCenter: parent.verticalCenter
                            checked: choice.modelData.value === "true"
                            onToggled: FileChooser.setChoice(choice.modelData.id, checked ? "false" : "true")
                        }

                        StyledText {
                            anchors.verticalCenter: parent.verticalCenter
                            text: choice.modelData.label
                            font.pointSize: Theme.font.size.small
                        }

                        Dropdown {
                            visible: !choice.toggle
                            fieldWidth: 160
                            options: choice.modelData.options.map(o => ({ value: o.id, label: o.label }))
                            value: choice.modelData.value || (choice.modelData.options[0]?.id ?? "")
                            onPicked: v => FileChooser.setChoice(choice.modelData.id, v)
                        }
                    }
                }

                // The GTK portal adds this when opening.
                Row {
                    visible: root.action === "open"
                    spacing: 8

                    Switch {
                        name: qsTr("Open files read-only")
                        anchors.verticalCenter: parent.verticalCenter
                        checked: FileChooser.readOnly
                        onToggled: FileChooser.readOnly = !FileChooser.readOnly
                    }

                    StyledText {
                        anchors.verticalCenter: parent.verticalCenter
                        text: qsTr("Open files read-only")
                        font.pointSize: Theme.font.size.small
                    }
                }
            }

            // Which files are shown (GTK: "Select which types of files are shown").
            Dropdown {
                id: filterBox

                visible: FileChooser.filters.length > 0
                anchors.right: buttons.left
                anchors.rightMargin: 16
                anchors.verticalCenter: parent.verticalCenter
                width: visible ? implicitWidth : 0
                fieldWidth: 200
                enabled: model.mode !== "search"
                options: FileChooser.filters.map((name, i) => ({ value: String(i), label: name }))
                value: String(FileChooser.filter)
                placeholder: qsTr("All Files")
                onPicked: v => FileChooser.filter = Number(v)
            }

            Row {
                id: buttons

                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                spacing: 10

                PillButton {
                    text: qsTr("Cancel")
                    onClicked: FileChooser.cancel()
                }

                PillButton {
                    // GTK: sensitive while there's something to answer with.
                    readonly property bool can: root.saving ? nameField.text.trim() !== ""
                                              : root.action === "folder" ? model.mode === "browse" || model.selection.length > 0
                                              : model.selection.length > 0 || (root.locating && locationField.text !== "")
                    primary: true
                    enabled: can
                    opacity: can ? 1 : 0.5
                    text: FileChooser.acceptLabel
                    onClicked: root.accept()
                }
            }
        }
    }

    // --- menus -------------------------------------------------------------------

    // The list's menu (GTK's file list popover) and the sidebar's.
    Item {
        id: menuLayer

        anchors.fill: parent
        visible: menu.shown  // not menu.visible: inside a hidden layer that is always false
        z: 10

        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.AllButtons
            onPressed: menu.shown = false
        }

        Menu {
            id: menu

            property string kind
            property var place

            function showFor(what: string, row: var, at: point): void {
                kind = what;
                place = row;
                actions = what === "list" ? listActions() : placeActions(row);
                origin = Qt.point(at.x + page.x, at.y + page.y);
                shown = true;
                forceActiveFocus();
            }

            function check(on: bool): string {
                return on ? "check" : "";
            }

            function listActions(): var {
                const sel = model.selection;
                const one = sel.length === 1;
                const folders = sel.length > 0 && model.allFolders;
                const out = [];
                if (model.mode !== "browse")
                    out.push({ icon: "subdirectory_arrow_right", text: qsTr("Visit File"), enabled: sel.length > 0, run: () => model.selectPath(sel[0]) });
                out.push({ icon: "folder_open", text: qsTr("Open With File Manager"), enabled: one && folders, run: () => FileChooser.openInFileManager(sel[0]) });
                out.push({ icon: "content_copy", text: qsTr("Copy Location"), enabled: sel.length > 0, run: () => FileChooser.copyLocations(sel) });
                out.push({ icon: "bookmark_add", text: qsTr("Add to Bookmarks"), enabled: folders, run: () => sel.forEach(p => places.addBookmark(p, -1)) });
                // Changing files is only offered when saving (GTK).
                if (root.saving && model.mode === "browse") {
                    out.push({ icon: "edit", text: qsTr("Rename…"), enabled: one, run: () => root.rename(sel[0]) });
                    out.push({ icon: "delete", text: qsTr("Move to Trash"), enabled: one, run: () => {
                        const r = FileChooser.trash(sel[0]);
                        if (r.error)
                            prompt.ask({ title: r.error, cancel: false });
                    } });
                }
                out.push("-");
                out.push({ text: qsTr("Show Hidden Files"), trailing: check(model.showHidden), run: () => root.remember("show_hidden", !model.showHidden) });
                out.push({ text: qsTr("Show Size Column"), trailing: check(list.showSize), run: () => root.remember("show_size", !list.showSize) });
                out.push({ text: qsTr("Show Type Column"), trailing: check(list.showType), run: () => root.remember("show_type", !list.showType) });
                out.push({ text: qsTr("Show Time"), trailing: check(model.showTime), run: () => root.remember("show_time", !model.showTime) });
                out.push({ text: qsTr("Sort Folders before Files"), trailing: check(model.foldersFirst), run: () => root.remember("folders_first", !model.foldersFirst) });
                return out;
            }

            function placeActions(row: var): var {
                const out = [{ icon: "folder_open", text: qsTr("Open"), enabled: row.kind !== "drive" || row.mounted, run: () => root.open(row.path) }];
                if (row.kind === "bookmark") {
                    out.push({ icon: "bookmark_remove", text: qsTr("Remove"), run: () => places.removeBookmark(row.path) });
                    out.push({ icon: "edit", text: qsTr("Rename…"), run: () => prompt.ask({
                        title: qsTr("Rename Bookmark"), field: true, text: row.name, accept: qsTr("Rename"),
                        run: t => places.renameBookmark(row.path, t)
                    }) });
                } else if (row.kind === "folder" || row.kind === "home") {
                    out.push({ icon: "bookmark_add", text: qsTr("Add Bookmark"), enabled: !places.isBookmarked(row.path), run: () => places.addBookmark(row.path, -1) });
                } else if (row.kind === "drive") {
                    out.push("-");
                    if (!row.mounted)
                        out.push({ icon: "hard_drive", text: qsTr("Mount"), run: () => places.mount(row.device) });
                    else {
                        out.push({ icon: "eject", text: qsTr("Unmount"), run: () => places.unmount(row.device) });
                        out.push({ icon: "eject", text: qsTr("Eject"), run: () => places.eject(row.device) });
                    }
                }
                return out;
            }

            property bool shown: false
            // Where it was asked for; kept inside the window once its size is known
            // (a binding: the rows lay out after showFor returns).
            property point origin
            x: Math.max(8, Math.min(origin.x, menuLayer.width - width - 8))
            y: Math.max(8, Math.min(origin.y, menuLayer.height - height - 8))
            onPicked: shown = false
            Keys.onEscapePressed: shown = false
        }
    }

    function rename(path: string): void {
        const name = path.slice(path.lastIndexOf("/") + 1);
        const dot = name.lastIndexOf(".");
        prompt.ask({
            title: qsTr("Name"),
            field: true,
            text: name,
            select: dot > 0 ? dot : name.length,
            accept: qsTr("Rename"),
            check: t => t === name ? {} : FileChooser.checkName(model.folder, t, model.resolve(path).isDir),
            run: t => {
                const r = FileChooser.rename(path, t);
                if (r.error)
                    prompt.ask({ title: qsTr("The file could not be renamed"), detail: r.error, cancel: false });
                else if (r.path)
                    model.selectPath(r.path);
            }
        });
    }

    Prompt {
        id: prompt

        z: 20
        onOpenChanged: if (!open) (root.saving ? nameField.focusField() : list.focusList())
    }

    // Saving: picking a file in the list puts its name in the field (its
    // name part picked, to type over), as GTK does; folders don't.
    Connections {
        target: model

        function onSelectionChanged(): void {
            if (!root.saving || model.selection.length !== 1 || model.allFolders)
                return;
            const p = model.selection[0];
            const name = p.slice(p.lastIndexOf("/") + 1);
            if (nameField.text === name)
                return;
            nameField.setText(name);
            const dot = name.lastIndexOf(".");
            nameField.select(0, dot > 0 ? dot : name.length);
        }
    }
}
