pragma ComponentBehavior: Bound

import QtQuick
import Atrium.Shell
import shell.components
import shell.modules.settings
import shell.services
import Atrium

// Opening or saving for an app, as a Mac's open and save panels: where it
// is on top (and the name to save under), the usual places on the left, the
// folder's files on the right; the app's file types and its own choices
// along the bottom. Closing the window or Cancel answers nothing.
FloatingWindow {
    id: root

    readonly property bool saving: FileChooser.mode !== "open"
    property string error
    property string confirm  // asked before replacing a file

    function accept(replace: bool): void {
        const next = FileChooser.accept(folderModel.selection, folderModel.folder, nameField.text, replace);
        error = next.error ?? "";
        confirm = next.confirm ?? "";
        if (next.enter)
            folderModel.folder = next.enter;
    }

    // A typed address: a folder is gone into, a file is the answer.
    function go(path: string): void {
        const r = folderModel.resolve(path);
        if (r.isDir) {
            folderModel.folder = r.path;
            pathBar.stop();
            list.forceActiveFocus();
        } else if (root.saving && r.path) {
            folderModel.folder = r.path.slice(0, r.path.lastIndexOf("/")) || "/";
            nameField.text = r.path.slice(r.path.lastIndexOf("/") + 1);
            pathBar.stop();
        } else if (r.allowed) {
            FileChooser.open([r.path]);
        } else {
            error = r.exists ? "That kind of file can't be opened here." : `There's no ${path.trim()}.`;
        }
    }

    title: FileChooser.title
    color: Theme.palette.windowBackground
    implicitWidth: 860
    implicitHeight: 580
    minimumSize: Qt.size(620, 420)
    onVisibleChanged: if (!visible) FileChooser.cancel()

    // Escape closes what was asked first, else the dialog (a typed address
    // takes its own Escape).
    Shortcut {
        sequence: "Escape"
        enabled: !pathBar.editing
        onActivated: {
            if (root.confirm !== "")
                root.confirm = "";
            else if (newFolderBox.visible)
                newFolderBox.visible = false;
            else
                FileChooser.cancel();
        }
    }

    FolderModel {
        id: folderModel

        folder: FileChooser.folder
        patterns: FileChooser.patterns
        foldersOnly: FileChooser.directory
        multiple: FileChooser.multiple
    }

    Item {
        anchors.fill: parent
        anchors.margins: 16

        // --- where, and the name to save under -----------------------------
        Row {
            id: saveRow

            visible: root.saving && FileChooser.mode === "save"
            height: visible ? 32 : 0
            spacing: 10
            anchors.horizontalCenter: parent.horizontalCenter

            StyledText {
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("Save As:")
            }

            Field {
                id: nameField

                width: 320
                text: FileChooser.name
                onAccepted: root.accept(false)
                Component.onCompleted: if (root.saving) {
                    focusField();
                    // The name picked, without its suffix, to type over.
                    const dot = text.lastIndexOf(".");
                    select(0, dot > 0 ? dot : text.length);
                }
            }
        }

        PathBar {
            id: pathBar

            anchors.top: saveRow.bottom
            anchors.topMargin: saveRow.visible ? 14 : 0
            width: parent.width
            model: folderModel
            onChose: path => root.go(path)
            onDone: list.forceActiveFocus()
        }

        // --- places and files ----------------------------------------------
        Item {
            id: middle

            anchors.top: pathBar.bottom
            anchors.topMargin: 12
            anchors.bottom: footer.top
            anchors.bottomMargin: 12
            width: parent.width

            Column {
                id: places

                width: 170
                spacing: 2

                Repeater {
                    model: folderModel.places

                    Rectangle {
                        id: place

                        required property var modelData
                        readonly property bool current: folderModel.folder === modelData.path

                        width: places.width
                        height: 30
                        radius: 7
                        color: current ? Theme.palette.secondaryFill : placeArea.containsMouse ? Theme.palette.tertiaryFill : "transparent"

                        Row {
                            x: 8
                            anchors.verticalCenter: parent.verticalCenter
                            spacing: 8

                            MaterialIcon {
                                anchors.verticalCenter: parent.verticalCenter
                                text: place.modelData.icon
                                color: Theme.palette.accent
                            }

                            StyledText {
                                anchors.verticalCenter: parent.verticalCenter
                                width: places.width - 44
                                elide: Text.ElideRight
                                text: place.modelData.name
                                font.pointSize: Theme.font.size.small
                            }
                        }

                        MouseArea {
                            id: placeArea

                            anchors.fill: parent
                            hoverEnabled: true
                            onClicked: root.go(place.modelData.path)
                        }
                    }
                }
            }

            Rectangle {
                anchors.left: places.right
                anchors.leftMargin: 12
                anchors.right: parent.right
                height: parent.height
                radius: 10
                color: Theme.palette.groupedBackground
                border.width: 1
                border.color: Theme.palette.separator
                clip: true

                // Column headings.
                Item {
                    id: heading

                    width: parent.width
                    height: 28

                    StyledText {
                        x: 44
                        anchors.verticalCenter: parent.verticalCenter
                        text: qsTr("Name")
                        font.pointSize: Theme.font.size.smaller
                        color: Theme.palette.secondaryLabel
                    }

                    StyledText {
                        x: parent.width - 290
                        anchors.verticalCenter: parent.verticalCenter
                        text: qsTr("Date Modified")
                        font.pointSize: Theme.font.size.smaller
                        color: Theme.palette.secondaryLabel
                    }

                    StyledText {
                        x: parent.width - 96
                        anchors.verticalCenter: parent.verticalCenter
                        text: qsTr("Size")
                        font.pointSize: Theme.font.size.smaller
                        color: Theme.palette.secondaryLabel
                    }

                    Rectangle {
                        anchors.bottom: parent.bottom
                        width: parent.width
                        height: 1
                        color: Theme.palette.separator
                    }
                }

                StyledText {
                    visible: folderModel.count === 0
                    anchors.centerIn: parent
                    text: FileChooser.directory ? qsTr("No folders here.") : qsTr("Nothing here.")
                    color: Theme.palette.secondaryLabel
                }

                ListView {
                    id: list

                    anchors.top: heading.bottom
                    anchors.bottom: parent.bottom
                    width: parent.width
                    anchors.margins: 4
                    clip: true
                    focus: !root.saving
                    model: folderModel
                    currentIndex: -1
                    boundsBehavior: Flickable.StopAtBounds

                    Connections {
                        target: folderModel

                        function onFolderChanged(): void {
                            list.currentIndex = -1;
                            list.positionViewAtBeginning();
                            root.error = "";
                        }
                    }

                    function step(by: int, extend: bool): void {
                        currentIndex = Math.max(0, Math.min(count - 1, currentIndex + by));
                        folderModel.select(currentIndex, extend ? FolderModel.Extend : FolderModel.Only);
                    }

                    Keys.onUpPressed: event => step(-1, event.modifiers & Qt.ShiftModifier)
                    Keys.onDownPressed: event => step(1, event.modifiers & Qt.ShiftModifier)
                    Keys.onReturnPressed: root.accept(false)
                    Keys.onEnterPressed: root.accept(false)
                    Keys.onPressed: event => {
                        if (event.key === Qt.Key_Backspace) {
                            folderModel.up();
                            event.accepted = true;
                        } else if (event.text === "/" || event.text === "~" || (event.key === Qt.Key_L && event.modifiers & Qt.ControlModifier)) {
                            pathBar.type(event.text === "~" ? "~/" : event.text === "/" ? "/" : folderModel.folder + "/");
                            event.accepted = true;
                        } else if (event.key === Qt.Key_H && event.modifiers & Qt.ControlModifier) {
                            folderModel.showHidden = !folderModel.showHidden;
                            event.accepted = true;
                        }
                    }

                    delegate: Rectangle {
                        id: row

                        required property int index
                        required property string path
                        required property string name
                        required property bool isDir
                        required property bool isImage
                        required property string icon
                        required property string size
                        required property string modified
                        required property bool selected

                        width: list.width
                        height: 30
                        radius: 6
                        color: selected ? Theme.palette.accent : rowArea.containsMouse ? Theme.palette.tertiaryFill : "transparent"
                        // Saving: only folders are there to go into.
                        opacity: root.saving && !isDir ? 0.45 : 1

                        // A picture shows itself; one that can't be read, its icon.
                        IconImage {
                            visible: !row.isImage || thumb.status === Image.Error
                            x: 12
                            anchors.verticalCenter: parent.verticalCenter
                            implicitSize: 20
                            source: Shell.iconPath(row.isDir ? "folder" : row.icon, "text-x-generic")
                            asynchronous: true
                        }

                        Image {
                            id: thumb

                            visible: row.isImage && status !== Image.Error
                            x: 12
                            anchors.verticalCenter: parent.verticalCenter
                            width: 20
                            height: 20
                            source: row.isImage ? "file://" + row.path : ""
                            sourceSize: Qt.size(40, 40)
                            fillMode: Image.PreserveAspectFit
                            asynchronous: true
                        }

                        StyledText {
                            x: 40
                            width: parent.width - 340
                            anchors.verticalCenter: parent.verticalCenter
                            elide: Text.ElideMiddle
                            text: row.name
                            font.pointSize: Theme.font.size.small
                            color: row.selected ? Theme.palette.labelOnAccent : Theme.palette.label
                        }

                        StyledText {
                            x: parent.width - 286
                            anchors.verticalCenter: parent.verticalCenter
                            text: row.modified
                            font.pointSize: Theme.font.size.smaller
                            color: row.selected ? Theme.palette.labelOnAccent : Theme.palette.secondaryLabel
                        }

                        StyledText {
                            x: parent.width - 92
                            anchors.verticalCenter: parent.verticalCenter
                            text: row.isDir ? "—" : row.size
                            font.pointSize: Theme.font.size.smaller
                            color: row.selected ? Theme.palette.labelOnAccent : Theme.palette.secondaryLabel
                        }

                        MouseArea {
                            id: rowArea

                            anchors.fill: parent
                            hoverEnabled: true
                            onClicked: mouse => {
                                list.currentIndex = row.index;
                                folderModel.select(row.index, mouse.modifiers & Qt.ControlModifier ? FolderModel.Toggle : mouse.modifiers & Qt.ShiftModifier ? FolderModel.Extend : FolderModel.Only);
                                if (root.saving && !row.isDir)
                                    nameField.text = row.name;
                                else
                                    list.forceActiveFocus();
                            }
                            onDoubleClicked: {
                                folderModel.select(row.index);
                                root.accept(false);
                            }
                        }
                    }
                }
            }
        }

        // --- the app's types and choices, and the answer -------------------
        Item {
            id: footer

            anchors.bottom: parent.bottom
            width: parent.width
            height: Math.max(buttons.height, options.height)

            Flow {
                id: options

                anchors.left: parent.left
                anchors.right: buttons.left
                anchors.rightMargin: 16
                anchors.verticalCenter: parent.verticalCenter
                spacing: 12

                Dropdown {
                    visible: FileChooser.filters.length > 1 && !FileChooser.directory
                    fieldWidth: 200
                    options: FileChooser.filters.map((name, i) => ({ value: String(i), label: name }))
                    value: String(FileChooser.filter)
                    onPicked: v => FileChooser.filter = Number(v)
                }

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
                            value: choice.modelData.value
                            onPicked: v => FileChooser.setChoice(choice.modelData.id, v)
                        }
                    }
                }
            }

            Row {
                id: buttons

                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                spacing: 10

                PillButton {
                    visible: root.saving || FileChooser.directory
                    text: qsTr("New Folder")
                    onClicked: {
                        newFolder.text = "untitled folder";
                        newFolderBox.visible = true;
                        newFolder.focusField();
                        newFolder.select(0, newFolder.text.length);
                    }
                }

                PillButton {
                    text: qsTr("Cancel")
                    onClicked: FileChooser.cancel()
                }

                PillButton {
                    primary: true
                    enabled: root.saving || FileChooser.directory || folderModel.selection.length > 0
                    opacity: enabled ? 1 : 0.5
                    text: FileChooser.acceptLabel
                    onClicked: root.accept(false)
                }
            }
        }

        StyledText {
            anchors.bottom: footer.top
            anchors.bottomMargin: 2
            anchors.right: parent.right
            visible: root.error !== ""
            text: root.error
            color: Theme.palette.red
            font.pointSize: Theme.font.size.smaller
        }
    }

    // --- asked first: a new folder's name, or replacing a file ---------------
    Rectangle {
        anchors.fill: parent
        visible: newFolderBox.visible || root.confirm !== ""
        color: Theme.palette.scrim

        MouseArea {
            anchors.fill: parent
        }
    }

    Rectangle {
        id: newFolderBox

        visible: false
        anchors.centerIn: parent
        width: 340
        height: newFolderColumn.implicitHeight + 40
        radius: 14
        color: Theme.palette.windowBackground
        border.width: 1
        border.color: Theme.palette.separator

        property string error

        function create(): void {
            const r = FileChooser.makeFolder(folderModel.folder, newFolder.text);
            error = r.error ?? "";
            if (r.path) {
                visible = false;
                folderModel.folder = r.path;
                list.forceActiveFocus();
            }
        }

        Column {
            id: newFolderColumn

            anchors.centerIn: parent
            width: parent.width - 40
            spacing: 12

            StyledText {
                text: qsTr("New Folder")
                font.weight: Font.DemiBold
            }

            Field {
                id: newFolder

                width: parent.width
                onAccepted: newFolderBox.create()
            }

            StyledText {
                visible: newFolderBox.error !== ""
                width: parent.width
                wrapMode: Text.Wrap
                text: newFolderBox.error
                color: Theme.palette.red
                font.pointSize: Theme.font.size.smaller
            }

            Row {
                anchors.right: parent.right
                spacing: 10

                PillButton {
                    text: qsTr("Cancel")
                    onClicked: newFolderBox.visible = false
                }

                PillButton {
                    primary: true
                    text: qsTr("Create")
                    onClicked: newFolderBox.create()
                }
            }
        }
    }

    Rectangle {
        visible: root.confirm !== ""
        anchors.centerIn: parent
        width: 380
        height: confirmColumn.implicitHeight + 40
        radius: 14
        color: Theme.palette.windowBackground
        border.width: 1
        border.color: Theme.palette.separator

        Column {
            id: confirmColumn

            anchors.centerIn: parent
            width: parent.width - 40
            spacing: 14

            StyledText {
                width: parent.width
                wrapMode: Text.Wrap
                text: root.confirm
                font.weight: Font.DemiBold
            }

            StyledText {
                width: parent.width
                wrapMode: Text.Wrap
                text: qsTr("Replacing it overwrites what's in it now.")
                color: Theme.palette.secondaryLabel
                font.pointSize: Theme.font.size.small
            }

            Row {
                anchors.right: parent.right
                spacing: 10

                PillButton {
                    text: qsTr("Cancel")
                    onClicked: root.confirm = ""
                }

                PillButton {
                    primary: true
                    text: qsTr("Replace")
                    onClicked: root.accept(true)
                }
            }
        }
    }
}
