pragma ComponentBehavior: Bound

import QtQuick
import Atrium.Shell
import shell.components
import shell.services
import Atrium

// GTK's file list: Name, Location (Recent and search), Size, Type and
// Modified (Accessed in Recent) columns, sorted by clicking a heading.
// A click picks (Ctrl adds, Shift extends), a double click or Return goes
// into a folder or answers with a file, a right click opens the menu.
// Rows drag out as files; files dropped in are gone to and picked.
FocusScope {
    id: root

    required property ChooserModel model
    property bool showSize: true
    property bool showType: true
    readonly property bool showLocation: model.mode !== "browse"
    readonly property alias currentIndex: list.currentIndex
    signal activated(int index)
    signal acceptRequested           // Return in a file mode: the dialog's default button
    signal menuAt(real x, real y)
    signal sortBy(string column)
    signal toSidebar
    signal dropped(var paths)

    function focusList(): void {
        list.forceActiveFocus();
    }

    function setCurrent(index: int): void {
        list.currentIndex = index;
        list.positionViewAtIndex(index, ListView.Contain);
    }

    // Column widths: Name takes what's left.
    readonly property real locationWidth: showLocation ? 170 : 0
    readonly property real sizeWidth: showSize ? 84 : 0
    readonly property real typeWidth: showType ? 110 : 0
    readonly property real timeWidth: model.showTime ? 150 : 110
    readonly property real nameWidth: width - 8 - locationWidth - sizeWidth - typeWidth - timeWidth

    Rectangle {
        anchors.fill: parent
        radius: 10
        color: Theme.palette.groupedBackground
        border.width: 1
        border.color: list.activeFocus ? Theme.palette.focusRing : Theme.palette.separator
        clip: true

        // Headings: a click sorts by that column, again turns it around.
        Row {
            id: heading

            x: 4
            height: 28
            visible: true

            Repeater {
                model: [
                    { key: "name", title: qsTr("Name"), width: root.nameWidth, shown: true },
                    { key: "", title: qsTr("Location"), width: root.locationWidth, shown: root.showLocation },
                    { key: "size", title: qsTr("Size"), width: root.sizeWidth, shown: root.showSize },
                    { key: "type", title: qsTr("Type"), width: root.typeWidth, shown: root.showType },
                    { key: "modified", title: root.model.mode === "recent" ? qsTr("Accessed") : qsTr("Modified"), width: root.timeWidth, shown: true }
                ]

                Rectangle {
                    id: head

                    required property var modelData
                    readonly property bool sorted: root.model.mode === "browse" && root.model.sortColumn === modelData.key

                    visible: modelData.shown
                    width: modelData.width
                    height: heading.height
                    color: headArea.containsMouse && modelData.key !== "" && root.model.mode === "browse" ? Theme.palette.quaternaryFill : "transparent"

                    StyledText {
                        x: head.modelData.key === "name" ? 36 : 6
                        anchors.verticalCenter: parent.verticalCenter
                        text: head.modelData.title
                        font.pointSize: Theme.font.size.smaller
                        font.weight: head.sorted ? Font.DemiBold : Font.Normal
                        color: Theme.palette.secondaryLabel
                    }

                    MaterialIcon {
                        visible: head.sorted
                        anchors.right: parent.right
                        anchors.rightMargin: 6
                        anchors.verticalCenter: parent.verticalCenter
                        text: root.model.ascending ? "arrow_drop_up" : "arrow_drop_down"
                        color: Theme.palette.secondaryLabel
                    }

                    MouseArea {
                        id: headArea

                        anchors.fill: parent
                        hoverEnabled: true
                        enabled: head.modelData.key !== "" && root.model.mode === "browse"
                        onClicked: root.sortBy(head.modelData.key)
                    }
                }
            }
        }

        Rectangle {
            anchors.top: heading.bottom
            width: parent.width
            height: 1
            color: Theme.palette.separator
        }

        Column {
            visible: list.count === 0 && root.model.mode === "search" && !root.model.searching && root.model.query !== ""
            anchors.centerIn: parent
            spacing: 6

            MaterialIcon {
                anchors.horizontalCenter: parent.horizontalCenter
                text: "search_off"
                font.pointSize: Theme.font.size.larger * 2
                color: Theme.palette.tertiaryLabel
            }

            StyledText {
                anchors.horizontalCenter: parent.horizontalCenter
                text: qsTr("No Results Found")
                font.weight: Font.DemiBold
            }

            StyledText {
                anchors.horizontalCenter: parent.horizontalCenter
                text: qsTr("Try a different search")
                color: Theme.palette.secondaryLabel
            }
        }

        StyledText {
            visible: root.model.error !== ""
            anchors.centerIn: parent
            text: root.model.error
            color: Theme.palette.secondaryLabel
        }

        ListView {
            id: list

            anchors.top: heading.bottom
            anchors.topMargin: 2
            anchors.bottom: parent.bottom
            anchors.bottomMargin: 2
            x: 4
            width: parent.width - 8
            clip: true
            focus: true
            model: root.model
            currentIndex: -1
            boundsBehavior: Flickable.StopAtBounds
            keyNavigationEnabled: false
            highlightFollowsCurrentItem: false
            Accessible.role: Accessible.List

            Connections {
                target: root.model

                function onFolderChanged(): void {
                    list.currentIndex = -1;
                    list.positionViewAtBeginning();
                }

                function onReveal(index: int): void {
                    root.setCurrent(index);
                }

                function onModeChanged(): void {
                    list.currentIndex = -1;
                }
            }

            function step(to: int, mods: int): void {
                if (count === 0)
                    return;
                to = Math.max(0, Math.min(count - 1, to));
                root.setCurrent(to);
                if (mods & Qt.ShiftModifier)
                    root.model.select(to, ChooserModel.Extend);
                else if (!(mods & Qt.ControlModifier))
                    root.model.select(to, ChooserModel.Only);
            }

            readonly property int page: Math.max(1, Math.floor(height / 30) - 1)

            Keys.onPressed: event => {
                const mods = event.modifiers;
                if (event.key === Qt.Key_Down) {
                    step(currentIndex + 1, mods);
                } else if (event.key === Qt.Key_Up) {
                    step(currentIndex < 0 ? 0 : currentIndex - 1, mods);
                } else if (event.key === Qt.Key_PageDown) {
                    step(currentIndex + page, mods);
                } else if (event.key === Qt.Key_PageUp) {
                    step(currentIndex - page, mods);
                } else if (event.key === Qt.Key_Home) {
                    step(0, mods);
                } else if (event.key === Qt.Key_End) {
                    step(count - 1, mods);
                } else if (event.key === Qt.Key_Space && (mods & Qt.ControlModifier)) {
                    root.model.select(currentIndex, ChooserModel.Toggle);
                } else if ((event.key === Qt.Key_A) && (mods & Qt.ControlModifier)) {
                    root.model.selectAll();
                } else if ((event.key === Qt.Key_Left || event.key === Qt.Key_Right) && !(mods & (Qt.ControlModifier | Qt.AltModifier | Qt.ShiftModifier))) {
                    root.toSidebar();
                } else if (event.key === Qt.Key_Menu || (event.key === Qt.Key_F10 && (mods & Qt.ShiftModifier))) {
                    const item = itemAtIndex(currentIndex);
                    const p = item ? item.mapToItem(root, item.width / 2, item.height / 2) : Qt.point(root.width / 2, root.height / 2);
                    root.menuAt(p.x, p.y);
                } else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter || event.key === Qt.Key_Space) {
                    // Choosing a folder, Return goes into the one under the cursor;
                    // otherwise it's the dialog's default button (GTK).
                    if (root.model.action === "folder" && currentIndex >= 0)
                        root.activated(currentIndex);
                    else
                        root.acceptRequested();
                } else {
                    return;
                }
                event.accepted = true;
            }

            delegate: Rectangle {
                id: rowItem

                required property int index
                required property string path
                required property string name
                required property bool isDir
                required property bool sensitive
                required property string icon
                required property string thumb
                required property string sizeText
                required property string type
                required property string dateText
                required property string timeText
                required property string location
                required property bool selected
                required property bool hidden

                width: list.width
                height: 30
                radius: 6
                color: selected ? Theme.palette.accent
                     : list.activeFocus && list.currentIndex === index ? Theme.palette.tertiaryFill
                     : rowArea.containsMouse ? Theme.palette.quaternaryFill
                     : "transparent"
                border.width: list.activeFocus && list.currentIndex === index && selected ? 1 : 0
                border.color: Theme.palette.focusRing
                // Choosing a folder: files are there to see, greyed.
                opacity: sensitive ? (hidden ? 0.75 : 1) : 0.4

                Accessible.role: Accessible.ListItem
                Accessible.name: name
                Accessible.selected: selected

                readonly property color text: selected ? Theme.palette.labelOnAccent : Theme.palette.label
                readonly property color dim: selected ? Theme.palette.labelOnAccent : Theme.palette.secondaryLabel

                // A thumbnail when there is one (or the picture itself), else the type's icon.
                Image {
                    id: thumbnail

                    visible: rowItem.thumb !== "" && status === Image.Ready
                    x: 8
                    anchors.verticalCenter: parent.verticalCenter
                    width: 20
                    height: 20
                    source: rowItem.thumb !== "" ? "file://" + rowItem.thumb : ""
                    sourceSize: Qt.size(40, 40)
                    fillMode: Image.PreserveAspectFit
                    asynchronous: true
                }

                IconImage {
                    visible: !thumbnail.visible
                    x: 8
                    anchors.verticalCenter: parent.verticalCenter
                    implicitSize: 20
                    source: Shell.iconPath(rowItem.isDir ? "folder" : rowItem.icon, "text-x-generic")
                    asynchronous: true
                }

                StyledText {
                    x: 36
                    width: root.nameWidth - 40
                    anchors.verticalCenter: parent.verticalCenter
                    elide: Text.ElideRight
                    text: rowItem.name
                    font.pointSize: Theme.font.size.small
                    color: rowItem.text
                }

                Row {
                    x: root.nameWidth
                    anchors.verticalCenter: parent.verticalCenter

                    StyledText {
                        visible: root.showLocation
                        width: root.locationWidth
                        leftPadding: 6
                        elide: Text.ElideMiddle
                        text: rowItem.location
                        font.pointSize: Theme.font.size.smaller
                        color: rowItem.dim
                    }

                    StyledText {
                        visible: root.showSize
                        width: root.sizeWidth
                        leftPadding: 6
                        text: rowItem.sizeText
                        font.pointSize: Theme.font.size.smaller
                        color: rowItem.dim
                    }

                    StyledText {
                        visible: root.showType
                        width: root.typeWidth
                        leftPadding: 6
                        elide: Text.ElideRight
                        text: rowItem.type
                        font.pointSize: Theme.font.size.smaller
                        color: rowItem.dim
                    }

                    StyledText {
                        width: root.timeWidth
                        leftPadding: 6
                        elide: Text.ElideRight
                        text: rowItem.timeText !== "" ? (rowItem.dateText !== "" ? rowItem.dateText + "  " : "") + rowItem.timeText : rowItem.dateText
                        font.pointSize: Theme.font.size.smaller
                        color: rowItem.dim
                    }
                }

                MouseArea {
                    id: rowArea

                    anchors.fill: parent
                    hoverEnabled: true
                    acceptedButtons: Qt.LeftButton | Qt.RightButton
                    drag.target: dragProxy
                    drag.threshold: 8

                    onPressed: mouse => {
                        list.forceActiveFocus();
                        list.currentIndex = rowItem.index;
                        if (mouse.button === Qt.RightButton) {
                            // GTK: a right click picks the row unless it's already picked.
                            if (!rowItem.selected)
                                root.model.select(rowItem.index, ChooserModel.Only);
                            const p = rowItem.mapToItem(root, mouse.x, mouse.y);
                            root.menuAt(p.x, p.y);
                            return;
                        }
                        const how = mouse.modifiers & Qt.ControlModifier ? ChooserModel.Toggle
                                  : mouse.modifiers & Qt.ShiftModifier ? ChooserModel.Extend
                                  : ChooserModel.Only;
                        // Pressing a row already picked among several keeps them all (a drag may follow).
                        if (how !== ChooserModel.Only || !rowItem.selected)
                            root.model.select(rowItem.index, how);
                    }
                    onClicked: mouse => {
                        if (mouse.button === Qt.LeftButton && !(mouse.modifiers & (Qt.ControlModifier | Qt.ShiftModifier)))
                            root.model.select(rowItem.index, ChooserModel.Only);
                    }
                    onDoubleClicked: mouse => {
                        if (mouse.button === Qt.LeftButton)
                            root.activated(rowItem.index);
                    }
                    onReleased: if (dragProxy.Drag.active) dragProxy.Drag.drop()
                }

                // Dragging rows out hands their files to whatever takes them.
                Item {
                    id: dragProxy

                    Drag.active: rowArea.drag.active
                    Drag.dragType: Drag.Automatic
                    Drag.supportedActions: Qt.CopyAction | Qt.MoveAction
                    Drag.mimeData: ({ "text/uri-list": (rowItem.selected ? root.model.selection : [rowItem.path]).map(p => "file://" + encodeURI(p)).join("\r\n") })
                }
            }

            // A file dropped in from elsewhere: its folder is gone to and it is picked.
            DropArea {
                anchors.fill: parent
                keys: ["text/uri-list"]
                onDropped: d => {
                    if (d.source)
                        return;  // our own rows
                    root.dropped(d.urls.map(u => decodeURIComponent(String(u).replace(/^file:\/\//, ""))));
                }
            }
        }
    }
}
