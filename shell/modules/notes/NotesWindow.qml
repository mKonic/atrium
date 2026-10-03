pragma ComponentBehavior: Bound

import QtQuick
import Atrium.Shell
import shell.components
import shell.services
import Atrium

// Notes: Markdown notes in one folder, the list on the left and the note on
// the right, drawn as it is written. Saves itself as you type; there is no
// Save. Ctrl+N a new note, Ctrl+P the search, Ctrl+Shift+Backspace to the trash.
FloatingWindow {
    id: root

    property string file: ""       // the note open ("" none)
    property bool dirty: false
    property string query: ""
    readonly property var shown: query ? notes.search(query) : notes.notes
    readonly property string noteTitle: file ? notes.titleFor(file, editor.text) : ""

    // "", "new", "search" or a note's file: what the palette asked for.
    function handle(what: string): void {
        visible = true;
        const self = Atrium.windows.find(w => w.app_id === "atrium-notes");
        if (self)
            Atrium.focusWindow(self.id);
        if (what === "new")
            newNote();
        else if (what === "search")
            search.forceActiveFocus();
        else if (what)
            open(what);
        else if (!file && notes.notes.length > 0)
            open(notes.notes[0].file);
        else
            editor.forceActiveFocus();
    }

    function flush(): void {
        if (file && dirty)
            notes.save(file, editor.text);
        dirty = false;
        saveTimer.stop();
    }

    function open(f: string): void {
        if (f === file)
            return;
        flush();
        file = f;
        editor.text = notes.load(f);
        dirty = false;
        editor.cursorPosition = editor.length;
        editor.forceActiveFocus();
    }

    function newNote(): void {
        flush();
        query = "";
        search.text = "";
        const f = notes.create();
        if (f)
            open(f);
    }

    function trashNote(): void {
        if (!file)
            return;
        const gone = file;
        const at = notes.notes.findIndex(n => n.file === gone);
        dirty = false;
        saveTimer.stop();
        file = "";
        editor.text = "";
        notes.trash(gone);
        const next = notes.notes[Math.min(at, notes.notes.length - 1)];
        if (next)
            open(next.file);
    }

    title: noteTitle || "Notes"
    visible: false
    titleBar: false
    color: Theme.palette.windowBackground
    implicitWidth: 900
    implicitHeight: 600
    minimumSize: Qt.size(560, 360)
    onVisibleChanged: if (!visible) flush()

    Notes {
        id: notes
    }

    Connections {
        target: Atrium

        function onShellAction(name: string): void {
            if (name === "notes" || name.startsWith("notes:"))
                root.handle(name.slice(6));
        }
    }

    Timer {
        id: saveTimer

        interval: 600
        onTriggered: root.flush()
    }

    Shortcut {
        sequence: "Ctrl+N"
        onActivated: root.newNote()
    }
    Shortcut {
        sequence: "Ctrl+P"
        onActivated: search.forceActiveFocus()
    }
    Shortcut {
        sequence: "Ctrl+Shift+Backspace"
        onActivated: root.trashNote()
    }

    // --- the list -----------------------------------------------------------------------
    Rectangle {
        id: sidebar

        readonly property int inset: 8

        x: inset
        y: inset
        width: 270 - inset
        height: parent.height - 2 * inset
        radius: 16
        color: Theme.light ? Qt.darker(Theme.palette.windowBackground, 1.035) : Qt.darker(Theme.palette.windowBackground, 1.16)
        border.width: 1
        border.color: Theme.light ? Qt.rgba(0, 0, 0, 0.09) : Qt.rgba(1, 1, 1, 0.14)

        MouseArea {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            height: toolbar.height - sidebar.inset
            onPressed: root.startMove()
            onDoubleClicked: root.toggleZoom()
        }

        Rectangle {
            id: searchBox

            x: 12
            y: 14
            width: parent.width - 24 - newButton.width - 8
            height: 34
            radius: 10
            color: Theme.palette.tertiaryFill

            MaterialIcon {
                id: glass

                anchors.left: parent.left
                anchors.leftMargin: 10
                anchors.verticalCenter: parent.verticalCenter
                text: "search"
                font.pointSize: Theme.font.size.normal
                color: Theme.palette.secondaryLabel
            }

            TextInput {
                id: search

                anchors.left: glass.right
                anchors.leftMargin: 6
                anchors.right: parent.right
                anchors.rightMargin: 10
                anchors.verticalCenter: parent.verticalCenter
                color: Theme.palette.label
                font.family: Theme.font.sans
                font.pointSize: Theme.font.size.normal
                clip: true
                onTextChanged: root.query = text
                Keys.onEscapePressed: {
                    text = "";
                    editor.forceActiveFocus();
                }
                Keys.onReturnPressed: {
                    if (root.shown.length > 0)
                        root.open(root.shown[0].file);
                }
                Keys.onDownPressed: noteList.forceActiveFocus()

                StyledText {
                    anchors.verticalCenter: parent.verticalCenter
                    visible: !search.text
                    text: "Search"
                    color: Theme.palette.tertiaryLabel
                }
            }
        }

        // A new note, next to the search as Apple Notes has it.
        Rectangle {
            id: newButton

            anchors.right: parent.right
            anchors.rightMargin: 12
            anchors.verticalCenter: searchBox.verticalCenter
            width: 34
            height: 34
            radius: 10
            color: newArea.containsMouse ? Theme.palette.tertiaryFill : "transparent"

            MaterialIcon {
                anchors.centerIn: parent
                text: "edit_square"
                font.pointSize: Theme.font.size.larger
                color: Theme.palette.secondaryLabel
            }

            MouseArea {
                id: newArea

                anchors.fill: parent
                hoverEnabled: true
                onClicked: root.newNote()
            }
        }

        ListView {
            id: noteList

            anchors.top: searchBox.bottom
            anchors.topMargin: 12
            anchors.bottom: parent.bottom
            anchors.bottomMargin: 8
            x: 8
            width: parent.width - 16
            clip: true
            spacing: 2
            model: root.shown
            boundsBehavior: Flickable.StopAtBounds
            keyNavigationEnabled: true
            Keys.onReturnPressed: if (currentItem) root.open(root.shown[currentIndex].file)

            delegate: Rectangle {
                id: entry

                required property var modelData
                required property int index
                readonly property bool current: modelData.file === root.file

                width: noteList.width
                height: 58
                radius: 9
                color: current ? Theme.palette.accentFill
                     : entryArea.containsMouse || (noteList.activeFocus && noteList.currentIndex === index)
                       ? Theme.palette.quaternaryFill : "transparent"

                Column {
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.margins: 12
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 3

                    StyledText {
                        width: parent.width
                        // The open one follows what's typed before it's saved.
                        text: entry.current ? (root.noteTitle || "New Note") : (entry.modelData.title || "New Note")
                        elide: Text.ElideRight
                        font.weight: Font.DemiBold
                    }

                    StyledText {
                        width: parent.width
                        text: entry.modelData.modified + (entry.modelData.preview ? "  " + entry.modelData.preview : "")
                        elide: Text.ElideRight
                        font.pointSize: Theme.font.size.small
                        color: Theme.palette.secondaryLabel
                    }
                }

                MouseArea {
                    id: entryArea

                    anchors.fill: parent
                    hoverEnabled: true
                    onClicked: root.open(entry.modelData.file)
                }
            }

            StyledText {
                anchors.centerIn: parent
                visible: noteList.count === 0
                text: root.query ? "No Results" : "No Notes"
                color: Theme.palette.secondaryLabel
            }
        }
    }

    // --- the toolbar ------------------------------------------------------------------------
    Item {
        id: toolbar

        anchors.left: sidebar.right
        anchors.leftMargin: sidebar.inset
        anchors.right: parent.right
        anchors.top: parent.top
        height: 56

        MouseArea {
            anchors.fill: parent
            onPressed: root.startMove()
            onDoubleClicked: root.toggleZoom()
        }

        StyledText {
            id: heading

            x: 32
            anchors.verticalCenter: parent.verticalCenter
            anchors.verticalCenterOffset: 2
            width: parent.width - 32 - 150
            visible: !renaming.visible
            elide: Text.ElideRight
            text: root.noteTitle || "Notes"
            font.pointSize: Theme.font.size.large
            font.weight: Font.Bold

            // Double-click names it.
            MouseArea {
                anchors.fill: parent
                enabled: root.file !== ""
                onDoubleClicked: {
                    renameField.text = notes.notes.find(n => n.file === root.file)?.name ?? "";
                    renaming.visible = true;
                    renameField.forceActiveFocus();
                    renameField.selectAll();
                }
            }
        }

        Rectangle {
            id: renaming

            x: 26
            anchors.verticalCenter: parent.verticalCenter
            width: Math.min(360, parent.width - 200)
            height: 32
            radius: 8
            visible: false
            color: Theme.palette.tertiaryFill
            border.width: 1
            border.color: Theme.palette.focusRing

            TextInput {
                id: renameField

                anchors.fill: parent
                anchors.leftMargin: 8
                anchors.rightMargin: 8
                verticalAlignment: TextInput.AlignVCenter
                color: Theme.palette.label
                font.family: Theme.font.sans
                font.pointSize: Theme.font.size.larger
                font.weight: Font.Bold
                clip: true
                onAccepted: {
                    root.flush();
                    root.file = notes.rename(root.file, text);
                    renaming.visible = false;
                    editor.forceActiveFocus();
                }
                Keys.onEscapePressed: {
                    renaming.visible = false;
                    editor.forceActiveFocus();
                }
                onActiveFocusChanged: if (!activeFocus) renaming.visible = false
            }
        }

        Row {
            anchors.right: lights.left
            anchors.rightMargin: 16
            anchors.verticalCenter: parent.verticalCenter
            spacing: 4

            Repeater {
                model: [["folder_open", "Open Folder", () => notes.openFolder()],
                        ["delete", "Move to Trash", () => root.trashNote()]]

                Rectangle {
                    required property var modelData

                    width: 32
                    height: 32
                    radius: 8
                    opacity: modelData[0] === "delete" && !root.file ? 0.4 : 1
                    color: toolArea.containsMouse ? Theme.palette.tertiaryFill : "transparent"

                    MaterialIcon {
                        anchors.centerIn: parent
                        text: parent.modelData[0]
                        font.pointSize: Theme.font.size.larger
                        color: Theme.palette.secondaryLabel
                    }

                    MouseArea {
                        id: toolArea

                        anchors.fill: parent
                        hoverEnabled: true
                        onClicked: parent.modelData[2]()
                    }
                }
            }
        }

        TrafficLights {
            id: lights

            anchors.right: parent.right
            anchors.rightMargin: 16
            anchors.verticalCenter: parent.verticalCenter
            anchors.verticalCenterOffset: 2
            window: root
            onCloseRequested: root.visible = false
        }
    }

    // --- the note -------------------------------------------------------------------------------
    Flickable {
        id: page

        anchors.left: sidebar.right
        anchors.leftMargin: sidebar.inset
        anchors.right: parent.right
        anchors.top: toolbar.bottom
        anchors.bottom: parent.bottom
        contentHeight: editor.implicitHeight + 40
        clip: true
        boundsBehavior: Flickable.StopAtBounds

        function follow(r: rect): void {
            if (r.y < contentY)
                contentY = r.y;
            else if (r.y + r.height + 24 > contentY + height)
                contentY = r.y + r.height + 24 - height;
        }

        TextEdit {
            id: editor

            x: 32
            y: 8
            width: page.width - 64
            visible: root.file !== ""
            wrapMode: TextEdit.Wrap
            color: Theme.palette.label
            selectionColor: Theme.palette.accent
            selectedTextColor: Theme.palette.labelOnAccent
            font.family: Theme.font.sans
            font.pointSize: Theme.font.size.larger
            selectByMouse: true
            persistentSelection: true
            textFormat: TextEdit.PlainText
            onTextChanged: {
                if (activeFocus || root.file) {
                    root.dirty = true;
                    saveTimer.restart();
                }
            }
            onCursorRectangleChanged: page.follow(cursorRectangle)

            // Enter carries a list on to the next line.
            Keys.onReturnPressed: event => {
                const n = notes.newline(text, cursorPosition);
                if (n.kind === "continue")
                    insert(cursorPosition, n.insert);
                else if (n.kind === "end")
                    remove(n.from, cursorPosition);
                else
                    event.accepted = false;
            }

            MarkdownHighlighter {
                document: editor.textDocument
                cursorPosition: editor.activeFocus ? editor.cursorPosition : -1
                text: Theme.palette.label
                secondary: Theme.palette.secondaryLabel
                accent: Theme.palette.accent
                codeBackground: Theme.palette.quaternaryFill
                monoFamily: Theme.font.mono
                baseSize: Theme.font.size.larger
            }

            StyledText {
                visible: editor.text.length === 0 && editor.preeditText.length === 0
                text: "Start writing. # for a heading, - for a list, **bold**, *italic*, `code`."
                font.pointSize: Theme.font.size.larger
                color: Theme.palette.tertiaryLabel
            }
        }

        // The page itself takes clicks below the text.
        MouseArea {
            y: editor.y + editor.height
            width: parent.width
            height: Math.max(0, page.height - y)
            onClicked: {
                editor.forceActiveFocus();
                editor.cursorPosition = editor.length;
            }
        }
    }

    Column {
        anchors.centerIn: page
        visible: root.file === ""
        spacing: 12

        MaterialIcon {
            anchors.horizontalCenter: parent.horizontalCenter
            text: "note_stack"
            font.pointSize: 32
            color: Theme.palette.tertiaryLabel
        }

        StyledText {
            anchors.horizontalCenter: parent.horizontalCenter
            text: "No note open"
            color: Theme.palette.secondaryLabel
        }

        DialogButton {
            anchors.horizontalCenter: parent.horizontalCenter
            text: "New Note"
            primary: true
            onClicked: root.newNote()
        }
    }
}
