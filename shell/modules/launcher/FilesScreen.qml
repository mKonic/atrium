pragma ComponentBehavior: Bound

import QtQuick
import Atrium.Shell
import shell.components
import shell.services
import Atrium

// Search Files: names under the folders Settings names, the selected one
// previewed on the right. Enter opens it; Ctrl+K has the rest.
Item {
    id: screen

    required property LauncherModel launcherModel
    property int current: 0
    readonly property var entries: search.results
    readonly property var selected: entries[current] ?? null
    readonly property var info: selected ? search.details(selected.path) : ({})
    readonly property var hints: [["Open", "Enter"], ["Actions", "Ctrl+K"]]
    // The header's type menu reads these.
    readonly property var filters: search.filters()
    readonly property string filter: search.filter
    signal done

    function setFilter(value: string): void {
        search.filter = value;
        current = 0;
    }

    function actions(query: string): var {
        if (!selected)
            return [];
        const all = [
            { id: "open", title: selected.folder ? qsTr("Open Folder") : qsTr("Open File"), glyph: "open_in_new", keys: "Enter", group: false },
            { id: "reveal", title: qsTr("Show in Files"), glyph: "folder_open", keys: "Ctrl+Enter", group: false },
            { id: "copy-file", title: selected.folder ? qsTr("Copy Folder") : qsTr("Copy File"), glyph: "content_copy", keys: "Ctrl+Shift+C", group: true },
            { id: "copy-path", title: qsTr("Copy Path"), glyph: "route", keys: "Ctrl+Alt+C", group: false },
            { id: "copy-name", title: qsTr("Copy Name"), glyph: "text_fields", keys: "", group: false },
            { id: "trash", title: qsTr("Move to Trash"), glyph: "delete", keys: "Ctrl+Delete", group: true },
        ];
        const q = query.toLowerCase();
        return q ? all.filter(a => a.title.toLowerCase().includes(q)) : all;
    }

    function runAction(id: string): void {
        if (!selected)
            return;
        const path = selected.path;
        if (id === "open") {
            done();
            search.open(path);
        } else if (id === "reveal") {
            done();
            search.reveal(path);
        } else if (id === "copy-file") {
            search.copyFile(path);
        } else if (id === "copy-path") {
            search.copyText(path);
        } else if (id === "copy-name") {
            search.copyText(selected.name);
        } else if (id === "trash") {
            search.trash(path);
        }
    }

    function handleKey(event: var): bool {
        const ctrl = event.modifiers & Qt.ControlModifier;
        const shift = event.modifiers & Qt.ShiftModifier;
        const alt = event.modifiers & Qt.AltModifier;
        if (event.key === Qt.Key_Down || (ctrl && event.key === Qt.Key_N))
            current = Math.min(entries.length - 1, current + 1);
        else if (event.key === Qt.Key_Up || (ctrl && event.key === Qt.Key_P))
            current = Math.max(0, current - 1);
        else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter)
            runAction(ctrl ? "reveal" : "open");
        else if (ctrl && shift && event.key === Qt.Key_C)
            runAction("copy-file");
        else if (ctrl && alt && event.key === Qt.Key_C)
            runAction("copy-path");
        else if (ctrl && event.key === Qt.Key_Delete)
            runAction("trash");
        else
            return false;
        list.positionViewAtIndex(current, ListView.Contain);
        return true;
    }

    onEntriesChanged: current = Math.min(current, Math.max(0, entries.length - 1))

    FileSearch {
        id: search

        query: screen.launcherModel.query
        onQueryChanged: screen.current = 0
    }

    StyledText {
        id: heading

        x: 20
        y: 10
        text: search.recent ? qsTr("Recently Used") : qsTr("Results")
        font.pointSize: Theme.font.size.small
        font.weight: Font.Medium
        color: Theme.palette.secondaryLabel
    }

    ListView {
        id: list

        anchors.top: heading.bottom
        anchors.topMargin: 6
        anchors.bottom: parent.bottom
        anchors.left: parent.left
        width: Math.round(parent.width * 0.42)
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
            height: 40

            Rectangle {
                anchors.fill: parent
                anchors.leftMargin: 8
                anchors.rightMargin: 4
                radius: 10
                color: row.selected ? Theme.palette.accentFill : "transparent"
            }

            IconImage {
                id: fileIcon

                anchors.left: parent.left
                anchors.leftMargin: 18
                anchors.verticalCenter: parent.verticalCenter
                implicitSize: 22
                source: Shell.iconPath(row.modelData.icon, row.modelData.genericIcon || "text-x-generic")
                asynchronous: true
            }

            StyledText {
                anchors.left: fileIcon.right
                anchors.leftMargin: 10
                anchors.right: parent.right
                anchors.rightMargin: 14
                anchors.verticalCenter: parent.verticalCenter
                // With the folder it's in: half of them are some "src" or "README".
                text: row.modelData.parent
                      ? `${row.modelData.name}<font color="${Theme.palette.tertiaryLabel}">  ${row.modelData.parent}</font>`
                      : row.modelData.name
                textFormat: Text.StyledText
                elide: Text.ElideMiddle
                font.pointSize: Theme.font.size.smaller
            }

            MouseArea {
                anchors.fill: parent
                hoverEnabled: true
                onPositionChanged: screen.current = row.index
                onClicked: screen.current = row.index
                onDoubleClicked: screen.runAction("open")
            }
        }
    }

    Rectangle {
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        anchors.left: list.right
        width: 1
        visible: screen.entries.length > 0
        color: Theme.palette.separator
    }

    // The file itself over what it is.
    Item {
        id: preview

        anchors.top: parent.top
        anchors.bottom: parent.bottom
        anchors.left: list.right
        anchors.right: parent.right
        anchors.margins: 16
        visible: screen.selected !== null

        Item {
            id: stage

            width: parent.width
            // 16:9, less what the information under it needs.
            height: Math.max(0, Math.min(Math.round(width * 9 / 16), preview.height - facts.implicitHeight - 12))
            clip: true

            Image {
                anchors.fill: parent
                visible: !!screen.info.image
                source: screen.info.image ?? ""
                sourceSize.width: 640
                fillMode: Image.PreserveAspectFit
                asynchronous: true
                smooth: true
            }

            Rectangle {
                anchors.fill: parent
                visible: !screen.info.image && !!screen.info.text
                radius: 8
                color: Theme.palette.quaternaryFill

                Text {
                    anchors.fill: parent
                    anchors.margins: 8
                    text: screen.info.text ?? ""
                    color: Theme.palette.label
                    font.family: Theme.font.mono
                    font.pointSize: Theme.font.size.small - 1
                    textFormat: Text.PlainText
                    wrapMode: Text.NoWrap
                    clip: true
                }
            }

            IconImage {
                anchors.centerIn: parent
                visible: !screen.info.image && !screen.info.text
                implicitSize: 96
                source: screen.info.icon ? Shell.iconPath(screen.info.icon, screen.info.genericIcon || "text-x-generic") : ""
            }
        }

        Column {
            id: facts

            anchors.top: stage.bottom
            anchors.topMargin: 12
            width: parent.width
            spacing: 5

            StyledText {
                width: parent.width
                text: screen.info.name ?? ""
                elide: Text.ElideMiddle
                font.weight: Font.Medium
            }

            Repeater {
                model: [["Where", screen.info.where], ["Type", screen.info.type], ["Size", screen.info.size],
                        ["Modified", screen.info.modified], ["Created", screen.info.created]].filter(r => r[1])

                Row {
                    required property var modelData

                    width: parent.width
                    spacing: 8

                    StyledText {
                        width: 70
                        text: parent.modelData[0]
                        font.pointSize: Theme.font.size.smaller
                        color: Theme.palette.secondaryLabel
                    }

                    StyledText {
                        width: parent.width - 78
                        text: parent.modelData[1]
                        elide: Text.ElideMiddle
                        font.pointSize: Theme.font.size.smaller
                    }
                }
            }
        }
    }

    StyledText {
        anchors.centerIn: parent
        visible: screen.entries.length === 0 && !search.searching
        text: search.recent ? qsTr("Type to search files and folders")
            : ({ all: "No files found", folders: "No folders found", documents: "No documents found",
                 images: "No images found", audio: "No audio found", videos: "No videos found",
                 archives: "No archives found" })[search.filter] ?? "No files found"
        color: Theme.palette.secondaryLabel
    }
}
