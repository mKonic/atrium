pragma ComponentBehavior: Bound

import QtQuick
import shell.components
import shell.services
import Atrium

// The places on the left, as GTK's file chooser lists them: Recent, Home,
// the usual folders, removable drives (a click mounts one; eject beside
// it), bookmarks, Other Locations. The one the list shows is highlighted.
// Right click: Open, Remove, Rename (bookmarks), Mount/Unmount/Eject.
// A folder dropped on it is bookmarked there.
Item {
    id: root

    required property ChooserPlaces places
    property string current  // a path, "recent" or "other"
    signal open(string location)
    signal menuAt(var row, real x, real y)
    signal error(string text)

    function focusList(): void {
        list.forceActiveFocus();
        if (list.currentIndex < 0)
            list.currentIndex = Math.max(0, root.places.rowFor(root.current));
    }

    ListView {
        id: list

        anchors.fill: parent
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        model: root.places.places
        currentIndex: -1
        keyNavigationEnabled: true
        activeFocusOnTab: true
        Keys.onReturnPressed: if (currentIndex >= 0) root.open(root.places.places[currentIndex].path)
        Keys.onRightPressed: root.open(root.places.places[currentIndex]?.path ?? root.current)

        section.property: "section"
        section.delegate: Item {
            required property string section

            width: list.width
            // Sections are set apart by a line, as GTK does, except the first.
            height: section === "computer" ? 0 : 13

            Rectangle {
                visible: parent.height > 0
                anchors.centerIn: parent
                width: parent.width - 16
                height: 1
                color: Theme.palette.separator
            }
        }

        delegate: Rectangle {
            id: place

            required property var modelData
            required property int index
            readonly property bool here: modelData.path === root.current
                                         || (modelData.kind === "drive" && modelData.path !== "" && root.current.startsWith(modelData.path + "/"))
            readonly property bool dropTarget: drop.containsDrag

            width: list.width
            height: 30
            radius: 7
            color: here ? Theme.palette.secondaryFill
                 : dropTarget || area.containsMouse || (list.activeFocus && list.currentIndex === index) ? Theme.palette.tertiaryFill
                 : "transparent"
            opacity: modelData.missing ? 0.5 : 1

            Accessible.role: Accessible.ListItem
            Accessible.name: modelData.name

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
                    width: place.width - 44 - (eject.visible ? eject.width : 0)
                    elide: Text.ElideRight
                    text: place.modelData.name
                    font.pointSize: Theme.font.size.small
                }
            }

            MouseArea {
                id: area

                anchors.fill: parent
                hoverEnabled: true
                acceptedButtons: Qt.LeftButton | Qt.RightButton | Qt.MiddleButton
                onClicked: mouse => {
                    list.currentIndex = place.index;
                    if (mouse.button === Qt.RightButton)
                        root.menuAt(place.modelData, place.mapToItem(root, mouse.x, mouse.y).x, place.mapToItem(root, mouse.x, mouse.y).y);
                    else if (place.modelData.kind === "drive" && !place.modelData.mounted)
                        root.places.mount(place.modelData.device);
                    else
                        root.open(place.modelData.path);
                }
            }

            // Removable drives that are mounted eject from here.
            Rectangle {
                id: eject

                visible: place.modelData.ejectable === true
                anchors.right: parent.right
                anchors.rightMargin: 4
                anchors.verticalCenter: parent.verticalCenter
                width: 24
                height: 24
                radius: 6
                color: ejectArea.containsMouse ? Theme.palette.secondaryFill : "transparent"

                MaterialIcon {
                    anchors.centerIn: parent
                    text: "eject"
                    color: Theme.palette.secondaryLabel
                }

                MouseArea {
                    id: ejectArea

                    anchors.fill: parent
                    hoverEnabled: true
                    onClicked: root.places.eject(place.modelData.device)
                }
            }

            // A folder dragged here from the list becomes a bookmark (after this row).
            DropArea {
                id: drop

                anchors.fill: parent
                keys: ["text/uri-list"]
                onDropped: d => {
                    for (const url of d.urls) {
                        const path = decodeURIComponent(String(url).replace(/^file:\/\//, ""));
                        root.places.addBookmark(path, -1);
                    }
                }
            }
        }
    }

    Connections {
        target: root.places

        function onMounted(device: string, path: string): void {
            root.open(path);
        }

        function onFailed(why: string): void {
            root.error(why);
        }
    }
}
