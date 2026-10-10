pragma ComponentBehavior: Bound

import QtQuick
import shell.components
import shell.services
import Atrium

// Search Notes: the Notes folder's notes, by title and text. Enter opens one
// in Notes; Ctrl+N starts a new one.
Item {
    id: screen

    required property LauncherModel launcherModel
    property int current: 0
    readonly property var entries: notes.search(launcherModel.query)
    readonly property var hints: [["Open Note", "Enter"], ["New Note", "Ctrl+N"]]
    signal done

    function openNote(file: string): void {
        done();
        Atrium.action("shell", file ? "notes:" + file : "notes:new");
    }

    function handleKey(event: var): bool {
        const ctrl = event.modifiers & Qt.ControlModifier;
        if (event.key === Qt.Key_Down)
            current = Math.min(entries.length - 1, current + 1);
        else if (event.key === Qt.Key_Up)
            current = Math.max(0, current - 1);
        else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter)
            openNote(entries[current]?.file ?? "");
        else if (ctrl && event.key === Qt.Key_N)
            openNote("");
        else
            return false;
        list.positionViewAtIndex(current, ListView.Contain);
        return true;
    }

    onEntriesChanged: current = 0

    Notes {
        id: notes
    }

    ListView {
        id: list

        anchors.fill: parent
        topMargin: 6
        bottomMargin: 6
        clip: true
        model: screen.entries
        currentIndex: screen.current
        boundsBehavior: Flickable.StopAtBounds

        delegate: Item {
            id: row

            required property var modelData
            required property int index

            width: list.width
            height: 54

            Rectangle {
                anchors.fill: parent
                anchors.leftMargin: 8
                anchors.rightMargin: 8
                radius: 10
                color: row.index === screen.current ? Theme.palette.accentFill : "transparent"
            }

            MaterialIcon {
                id: icon

                anchors.left: parent.left
                anchors.leftMargin: 22
                anchors.verticalCenter: parent.verticalCenter
                text: "sticky_note_2"
                font.pointSize: 16
                color: Theme.palette.yellow
            }

            Column {
                anchors.left: icon.right
                anchors.leftMargin: 12
                anchors.right: parent.right
                anchors.rightMargin: 20
                anchors.verticalCenter: parent.verticalCenter
                spacing: 2

                StyledText {
                    width: parent.width
                    text: row.modelData.title || qsTr("New Note")
                    elide: Text.ElideRight
                }

                StyledText {
                    width: parent.width
                    text: row.modelData.modified + (row.modelData.preview ? "  " + row.modelData.preview : "")
                    elide: Text.ElideRight
                    font.pointSize: Theme.font.size.small
                    color: Theme.palette.tertiaryLabel
                }
            }

            MouseArea {
                anchors.fill: parent
                hoverEnabled: true
                onPositionChanged: screen.current = row.index
                onClicked: screen.openNote(row.modelData.file)
            }
        }

        StyledText {
            anchors.centerIn: parent
            visible: list.count === 0
            text: screen.launcherModel.query ? qsTr("No Results") : qsTr("No Notes yet: Ctrl+N starts one")
            color: Theme.palette.secondaryLabel
        }
    }
}
