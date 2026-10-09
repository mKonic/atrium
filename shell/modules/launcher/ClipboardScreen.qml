pragma ComponentBehavior: Bound

import QtQuick
import shell.components
import shell.services
import Atrium

// Clipboard History: what you copied, filtered by the palette's field and
// its type, the whole of the selected one on the right. Enter pastes it where
// you were typing; Ctrl+Enter only copies it. Pins (Ctrl+.) stay on top,
// past the limit and Clear All; Ctrl+1..0 pastes one (Tinycast's).
Item {
    id: screen

    required property LauncherModel launcherModel
    property int current: 0
    readonly property var entries: Clipboard.results
    readonly property var selected: entries[current] ?? null
    // The opening selection lands past the pins, once the list is in.
    property bool landed: false
    // A row pinned or unpinned moves: the selection goes with it.
    property string following: ""

    function togglePin(): void {
        if (!selected)
            return;
        following = selected.id;
        Clipboard.togglePin(selected.id);
    }
    signal done
    // Something to confirm first ({ title, detail, glyph, run }): the palette asks.
    signal confirm(var request)

    // The header's type filter (the palette shows it as a pop-up).
    readonly property var filters: Clipboard.filters
    readonly property string filter: Clipboard.filter

    function setFilter(value: string): void {
        landed = false;
        Clipboard.filter = value;
    }

    function clearAll(): void {
        if (Clipboard.results.length === 0)
            return;
        screen.confirm({
            title: "Clear Clipboard History?",
            detail: "Everything you copied is forgotten but the pins. What's on the clipboard now stays.",
            glyph: "delete_sweep",
            run: () => Clipboard.clear()
        });
    }

    function paste(copyOnly: bool): void {
        if (!selected)
            return;
        Clipboard.copy(selected.id);
        const text = selected.image ? "" : (Clipboard.preview.full ?? Clipboard.preview.text ?? "");
        done();
        if (!copyOnly && text.length > 0)
            Atrium.insertText(text);
    }

    // Ctrl+1..0: the pins as they're listed, the first ten.
    function pastePin(n: int): void {
        if (Clipboard.pinAt(n) === "")
            return;
        current = n;
        paste(false);
    }

    function actions(query: string): var {
        if (!selected)
            return [];
        const all = [
            { id: "paste", title: "Paste", glyph: "content_paste", keys: "Enter", group: false },
            { id: "copy", title: "Copy", glyph: "content_copy", keys: "Ctrl+Enter", group: false },
            { id: "pin", title: selected.pinned ? "Unpin Entry" : "Pin Entry",
              glyph: selected.pinned ? "keep_off" : "keep", keys: "Ctrl+.", group: true },
            { id: "delete", title: "Delete Entry", glyph: "delete", keys: "Shift+Delete", group: true },
            { id: "clear", title: "Clear All", glyph: "delete_sweep", keys: "Ctrl+Shift+Delete", group: false },
        ];
        const q = query.toLowerCase();
        return q ? all.filter(a => a.title.toLowerCase().includes(q)) : all;
    }

    function runAction(id: string): void {
        if (id === "paste")
            paste(false);
        else if (id === "copy")
            paste(true);
        else if (id === "pin")
            togglePin();
        else if (id === "delete" && selected)
            Clipboard.remove(selected.id);
        else if (id === "clear")
            clearAll();
    }

    function handleKey(event: var): bool {
        const ctrl = event.modifiers & Qt.ControlModifier;
        if (ctrl && event.key >= Qt.Key_0 && event.key <= Qt.Key_9)
            pastePin(event.key === Qt.Key_0 ? 9 : event.key - Qt.Key_1);
        else if (ctrl && event.key === Qt.Key_Period)
            togglePin();
        else if (event.key === Qt.Key_Down || (ctrl && event.key === Qt.Key_N))
            current = Math.min(entries.length - 1, current + 1);
        else if (event.key === Qt.Key_Up || (ctrl && event.key === Qt.Key_P))
            current = Math.max(0, current - 1);
        else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter)
            paste(ctrl);
        else if (ctrl && (event.modifiers & Qt.ShiftModifier) && event.key === Qt.Key_Delete)
            clearAll();
        else if ((event.modifiers & Qt.ShiftModifier) && event.key === Qt.Key_Delete && selected)
            Clipboard.remove(selected.id);
        else
            return false;
        list.positionViewAtIndex(current, ListView.Contain);
        return true;
    }

    // The footer's words for Enter and the rest.
    // Pin, Delete and Clear All are in the actions (Ctrl+K).
    readonly property var hints: [["Paste", "Enter"], ["Copy", "Ctrl+Enter"], ["Actions", "Ctrl+K"]]

    Component.onCompleted: {
        Clipboard.query = launcherModel.query;
        Clipboard.refresh();
    }
    onSelectedChanged: if (selected) Clipboard.showPreview(selected.id)
    onEntriesChanged: {
        if (following !== "" && Clipboard.indexOf(following) >= 0) {
            current = Clipboard.indexOf(following);
            following = "";
            list.positionViewAtIndex(current, ListView.Contain);
        }
        if (!landed && entries.length > 0) {
            current = Clipboard.landing;
            landed = true;
        }
        current = Math.min(current, Math.max(0, entries.length - 1));
    }

    Connections {
        target: screen.launcherModel

        function onQueryChanged(): void {
            Clipboard.query = screen.launcherModel.query;
            screen.current = 0;
        }
    }

    ListView {
        id: list

        anchors.top: parent.top
        anchors.bottom: parent.bottom
        anchors.left: parent.left
        width: Math.round(parent.width * 0.42)
        topMargin: 6
        bottomMargin: 6
        clip: true
        model: screen.entries
        currentIndex: screen.current
        boundsBehavior: Flickable.StopAtBounds
        reuseItems: true

        delegate: Item {
            id: row

            required property var modelData
            required property int index
            readonly property bool selected: index === screen.current

            width: list.width
            height: modelData.image ? 64 : 44

            Rectangle {
                anchors.fill: parent
                anchors.leftMargin: 8
                anchors.rightMargin: 4
                radius: 10
                color: row.selected ? Theme.palette.accentFill : "transparent"
            }

            MaterialIcon {
                anchors.left: parent.left
                anchors.leftMargin: 20
                anchors.verticalCenter: parent.verticalCenter
                visible: !row.modelData.color && (!row.modelData.image || !row.modelData.thumb)
                text: ({ image: "image", file: "description", link: "link", email: "alternate_email" })[row.modelData.kind] ?? "notes"
                color: Theme.palette.secondaryLabel
            }

            // A copied colour, drawn.
            Rectangle {
                anchors.left: parent.left
                anchors.leftMargin: 21
                anchors.verticalCenter: parent.verticalCenter
                visible: !!row.modelData.color
                width: 18
                height: 18
                radius: 5
                color: row.modelData.color || "transparent"
                border.width: 1
                border.color: Theme.palette.separator
            }

            Image {
                anchors.left: parent.left
                anchors.leftMargin: 18
                anchors.verticalCenter: parent.verticalCenter
                width: 72
                height: 48
                visible: row.modelData.image && !!row.modelData.thumb
                source: row.modelData.thumb ?? ""
                sourceSize.width: 144
                sourceSize.height: 96
                fillMode: Image.PreserveAspectFit
                asynchronous: true
            }

            StyledText {
                anchors.left: parent.left
                anchors.leftMargin: row.modelData.image ? 100 : 48
                anchors.right: parent.right
                anchors.rightMargin: row.modelData.pinned ? 40 : 16
                anchors.verticalCenter: parent.verticalCenter
                text: row.modelData.image ? `${row.modelData.width}×${row.modelData.height} ${row.modelData.format.toUpperCase()} · ${row.modelData.size}`
                                          : row.modelData.text.trim()
                elide: Text.ElideRight
                maximumLineCount: 1
                font.pointSize: Theme.font.size.smaller
                color: row.modelData.image ? Theme.palette.secondaryLabel : Theme.palette.label
            }

            MaterialIcon {
                anchors.right: parent.right
                anchors.rightMargin: 16
                anchors.verticalCenter: parent.verticalCenter
                visible: row.modelData.pinned
                text: "keep"
                font.pointSize: Theme.font.size.small
                color: row.selected ? Theme.palette.label : Theme.palette.secondaryLabel
            }

            MouseArea {
                anchors.fill: parent
                hoverEnabled: true
                onPositionChanged: screen.current = row.index
                onClicked: {
                    screen.current = row.index;
                    screen.paste(false);
                }
            }
        }
    }

    Rectangle {
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        anchors.left: list.right
        width: 1
        color: Theme.palette.separator
    }

    Item {
        id: previewPane

        readonly property var p: Clipboard.preview

        anchors.top: parent.top
        anchors.bottom: parent.bottom
        anchors.left: list.right
        anchors.right: parent.right
        anchors.margins: 18
        clip: true

        Image {
            anchors.fill: parent
            visible: previewPane.p.image === true
            source: previewPane.p.image ? previewPane.p.thumb : ""
            fillMode: Image.PreserveAspectFit
            asynchronous: true
            smooth: true
            mipmap: true
        }

        Flickable {
            anchors.fill: parent
            visible: previewPane.p.image === false
            contentHeight: full.implicitHeight
            boundsBehavior: Flickable.StopAtBounds

            Text {
                id: full

                width: parent.width
                text: previewPane.p.full ?? previewPane.p.text ?? ""
                wrapMode: Text.Wrap
                color: Theme.palette.label
                font.family: Theme.font.mono
                font.pointSize: Theme.font.size.smaller
                textFormat: Text.PlainText
            }
        }

        Column {
            anchors.centerIn: parent
            visible: screen.entries.length === 0
            spacing: 8

            MaterialIcon {
                anchors.horizontalCenter: parent.horizontalCenter
                text: "content_paste_off"
                font.pointSize: 26
                color: Theme.palette.tertiaryLabel
            }

            StyledText {
                anchors.horizontalCenter: parent.horizontalCenter
                text: !Clipboard.available ? "Clipboard history isn't kept here"
                    : screen.launcherModel.query.length > 0 ? "No Results" : Clipboard.emptyText
                color: Theme.palette.secondaryLabel
            }
        }
    }
}
