pragma ComponentBehavior: Bound

import QtQuick
import Atrium.Shell
import shell.components
import shell.services
import Atrium

// Choosing a file, as macOS's open panel does it, in a sheet: places on the
// left, the folder's files on the right (pictures as thumbnails), and a path
// bar that takes a typed address ("~/Pictures", "/usr/share/backgrounds",
// or a file). Double-click or Return opens a folder or picks a file.
Sheet {
    id: root

    property string nameFilter: ""
    property string folder: ""  // where it starts (a path or a file:// URL)
    property string selected: ""
    property bool editingPath: false
    signal picked(url file)

    readonly property bool selectedAllowed: selected !== "" && folderModel.resolve(selected).allowed

    function choose(path: string): void {
        const r = folderModel.resolve(path);
        if (r.isDir) {
            folderModel.folder = r.path;
            selected = "";
            editingPath = false;
            error = "";
            grid.forceActiveFocus();
        } else if (r.allowed) {
            picked("file://" + r.path);
            close();
        } else if (!r.exists) {
            error = `There's no ${path.trim()}.`;
        } else {
            error = "That kind of file can't be used here.";
        }
    }

    width: 780
    action: "Open"
    ready: selectedAllowed
    onOpened: {
        if (folder)
            folderModel.folder = folder.replace(/^file:\/\//, "");
        selected = "";
        editingPath = false;
        grid.forceActiveFocus();
    }
    onSubmitted: choose(selected)

    FolderModel {
        id: folderModel

        filter: root.nameFilter
    }

    // --- the path bar ------------------------------------------------------
    Row {
        width: parent.width
        spacing: 8

        Rectangle {
            width: 32
            height: 32
            radius: 8
            color: upArea.containsMouse ? Theme.palette.secondaryFill : Theme.palette.tertiaryFill
            opacity: folderModel.folder !== "/" ? 1 : 0.4

            MaterialIcon {
                anchors.centerIn: parent
                text: "arrow_upward"
                color: Theme.palette.label
            }

            MouseArea {
                id: upArea

                anchors.fill: parent
                hoverEnabled: true
                onClicked: folderModel.up()
            }
        }

        Rectangle {
            id: bar

            width: parent.width - 40
            height: 32
            radius: 8
            color: Theme.palette.tertiaryFill
            border.width: 1
            border.color: pathInput.activeFocus ? Theme.palette.focusRing : "transparent"
            clip: true

            // Clicking beside the crumbs types an address instead.
            MouseArea {
                anchors.fill: parent
                cursorShape: Qt.IBeamCursor
                onClicked: {
                    pathInput.text = folderModel.folder === "/" ? "/" : folderModel.folder + "/";
                    root.editingPath = true;
                    pathInput.forceActiveFocus();
                }
            }

            Row {
                visible: !root.editingPath
                x: 6
                anchors.verticalCenter: parent.verticalCenter
                spacing: 0

                Repeater {
                    model: folderModel.crumbs

                    Row {
                        id: crumb

                        required property var modelData
                        required property int index

                        MaterialIcon {
                            visible: crumb.index > 0
                            anchors.verticalCenter: parent.verticalCenter
                            text: "chevron_right"
                            font.pointSize: Theme.font.size.small
                            color: Theme.palette.tertiaryLabel
                        }

                        Rectangle {
                            width: crumbText.implicitWidth + 12
                            height: 24
                            radius: 6
                            color: crumbArea.containsMouse ? Theme.palette.secondaryFill : "transparent"

                            StyledText {
                                id: crumbText

                                anchors.centerIn: parent
                                text: crumb.index === 0 ? "Computer" : crumb.modelData.name
                                font.pointSize: Theme.font.size.small
                                font.weight: crumb.index === folderModel.crumbs.length - 1 ? Font.DemiBold : Font.Normal
                                color: crumb.index === folderModel.crumbs.length - 1 ? Theme.palette.label : Theme.palette.secondaryLabel
                            }

                            MouseArea {
                                id: crumbArea

                                anchors.fill: parent
                                hoverEnabled: true
                                onClicked: root.choose(crumb.modelData.path)
                            }
                        }
                    }
                }
            }

            TextInput {
                id: pathInput

                visible: root.editingPath
                anchors.fill: parent
                anchors.leftMargin: 10
                anchors.rightMargin: 10
                verticalAlignment: TextInput.AlignVCenter
                color: Theme.palette.label
                font.family: Theme.font.sans
                font.pointSize: Theme.font.size.small
                selectByMouse: true
                onAccepted: root.choose(text)
                onActiveFocusChanged: if (!activeFocus) root.editingPath = false
                Keys.onEscapePressed: {
                    root.editingPath = false;
                    grid.forceActiveFocus();
                }
            }
        }
    }

    // --- places and files ---------------------------------------------------
    Row {
        width: parent.width
        height: 380
        spacing: 12

        Column {
            width: 160
            spacing: 2

            Repeater {
                model: folderModel.places

                Rectangle {
                    id: place

                    required property var modelData
                    readonly property bool current: folderModel.folder === modelData.path

                    width: 160
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
                            width: 120
                            elide: Text.ElideRight
                            text: place.modelData.name
                            font.pointSize: Theme.font.size.small
                        }
                    }

                    MouseArea {
                        id: placeArea

                        anchors.fill: parent
                        hoverEnabled: true
                        onClicked: root.choose(place.modelData.path)
                    }
                }
            }
        }

        Rectangle {
            width: parent.width - 172
            height: parent.height
            radius: 10
            color: Theme.palette.groupedBackground
            border.width: 1
            border.color: Theme.palette.separator
            clip: true

            StyledText {
                visible: folderModel.count === 0
                anchors.centerIn: parent
                text: "Nothing here to choose."
                color: Theme.palette.secondaryLabel
            }

            GridView {
                id: grid

                anchors.fill: parent
                anchors.margins: 8
                cellWidth: Math.floor(width / Math.max(1, Math.floor(width / 116)))
                cellHeight: 118
                model: folderModel
                clip: true
                boundsBehavior: Flickable.StopAtBounds
                currentIndex: -1
                keyNavigationEnabled: true
                onCurrentIndexChanged: if (currentItem) root.selected = currentItem.path

                Connections {
                    target: folderModel

                    function onFolderChanged(): void {
                        grid.currentIndex = -1;
                        grid.positionViewAtBeginning();
                    }
                }

                Keys.onReturnPressed: if (root.selected) root.choose(root.selected)
                Keys.onEnterPressed: if (root.selected) root.choose(root.selected)
                Keys.onPressed: event => {
                    if (event.key === Qt.Key_Backspace || (event.key === Qt.Key_Up && event.modifiers & Qt.AltModifier)) {
                        folderModel.up();
                        event.accepted = true;
                    } else if (event.text === "/" || event.text === "~" || (event.key === Qt.Key_L && event.modifiers & Qt.ControlModifier)) {
                        pathInput.text = event.text === "~" ? "~/" : event.text === "/" ? "/" : folderModel.folder + "/";
                        root.editingPath = true;
                        pathInput.forceActiveFocus();
                        event.accepted = true;
                    }
                }

                delegate: Item {
                    id: file

                    required property int index
                    required property string path
                    required property string name
                    required property bool isDir
                    required property bool isImage
                    required property string icon
                    required property string size
                    readonly property bool current: GridView.isCurrentItem

                    width: grid.cellWidth
                    height: grid.cellHeight

                    Rectangle {
                        anchors.fill: parent
                        anchors.margins: 3
                        radius: 9
                        color: file.current ? Theme.palette.accent : fileArea.containsMouse ? Theme.palette.tertiaryFill : "transparent"
                        opacity: file.current ? 0.3 : 1
                    }

                    Item {
                        id: thumb

                        x: (parent.width - 72) / 2
                        y: 8
                        width: 72
                        height: 64

                        Image {
                            visible: file.isImage
                            anchors.fill: parent
                            source: file.isImage ? "file://" + file.path : ""
                            sourceSize: Qt.size(144, 128)
                            fillMode: Image.PreserveAspectFit
                            asynchronous: true
                            smooth: true
                        }

                        IconImage {
                            visible: !file.isImage
                            anchors.centerIn: parent
                            implicitSize: 56
                            source: Shell.iconPath(file.isDir ? "folder" : file.icon, "text-x-generic")
                            asynchronous: true
                        }
                    }

                    StyledText {
                        anchors.top: thumb.bottom
                        anchors.topMargin: 6
                        x: 6
                        width: parent.width - 12
                        horizontalAlignment: Text.AlignHCenter
                        elide: Text.ElideMiddle
                        text: file.name
                        font.pointSize: Theme.font.size.smaller
                    }

                    MouseArea {
                        id: fileArea

                        anchors.fill: parent
                        hoverEnabled: true
                        onClicked: {
                            grid.currentIndex = file.index;
                            grid.forceActiveFocus();
                        }
                        onDoubleClicked: root.choose(file.path)
                    }
                }
            }
        }
    }

    StyledText {
        width: parent.width
        elide: Text.ElideMiddle
        text: root.selected ? root.selected.slice(root.selected.lastIndexOf("/") + 1) : "Type / or ~ to go to an address."
        font.pointSize: Theme.font.size.smaller
        color: Theme.palette.secondaryLabel
    }
}
