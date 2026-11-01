pragma ComponentBehavior: Bound

import QtQuick
import Atrium.Shell
import shell.components
import shell.services
import Atrium

// A folder's newest files fanned up out of its Dock icon, as a Mac's stack
// in Fan view: the newest nearest the Dock, each with its name beside it, an
// Open in Files button on top. Click one to open it, drag one out to drop
// it somewhere, Escape or a click elsewhere to fold it.
Item {
    id: root

    property DockFolder folder: null
    property bool open: false
    readonly property int rows: Math.min(folder?.items.length ?? 0, 10)
    readonly property real rowHeight: 58

    signal closed

    width: 360
    height: (rows + 1) * rowHeight + 24
    visible: open || fold.running

    // The fan unfolds from the icon and folds back into it.
    property real unfolded: open ? 1 : 0

    Behavior on unfolded {
        Anim {
            id: fold

            duration: Theme.anim.normal
            easing.bezierCurve: Theme.anim.emphasizedDecel
        }
    }

    // Open in Files, at the top, and how many more there are.
    Rectangle {
        id: header

        anchors.right: parent.right
        anchors.rightMargin: 4 - root.rows * root.rows * 0.9 * root.unfolded
        y: root.height - (root.rows + 1) * root.rowHeight * root.unfolded - 8
        rotation: root.rows * 1.1 * root.unfolded
        transformOrigin: Item.Right
        width: headerText.implicitWidth + 28
        height: 32
        radius: height / 2
        color: headerArea.containsMouse ? Theme.material.thick : Theme.material.regular
        opacity: root.unfolded
        border.width: Theme.lens ? 0 : 1
        border.color: Theme.palette.separator

        Glass {
            material: headerArea.containsMouse ? GlassShape.Thick : GlassShape.Regular
        }

        StyledText {
            id: headerText

            anchors.centerIn: parent
            text: (root.folder?.count ?? 0) > root.rows
                  ? qsTr("Open in Files (%1 more)").arg(root.folder.count - root.rows)
                  : root.rows === 0 ? qsTr("No items · Open in Files") : qsTr("Open in Files")
            font.weight: Font.Medium
        }

        MouseArea {
            id: headerArea

            anchors.fill: parent
            hoverEnabled: true
            onClicked: {
                root.folder.openFolder();
                root.closed();
            }
        }
    }

    Repeater {
        model: root.folder ? root.folder.items.slice(0, root.rows) : []

        Item {
            id: entry

            required property var modelData
            required property int index
            // Up the fan, curving right and leaning as it goes, as on a Mac.
            readonly property real step: index + 1

            anchors.right: parent.right
            anchors.rightMargin: -index * index * 0.9 * root.unfolded
            y: root.height - step * root.rowHeight * root.unfolded - 8
            width: parent.width
            height: root.rowHeight
            rotation: index * 1.1 * root.unfolded
            transformOrigin: Item.Right
            opacity: Math.min(1, root.unfolded * 1.6 - index * 0.04)

            Rectangle {
                anchors.right: thumb.left
                anchors.rightMargin: 10
                anchors.verticalCenter: thumb.verticalCenter
                width: Math.min(name.implicitWidth + 20, 250)
                height: 26
                radius: height / 2
                color: area.containsMouse ? Theme.palette.accent : Theme.material.regular
                border.width: Theme.lens || area.containsMouse ? 0 : 1
                border.color: Theme.palette.separator

                Glass {
                    visible: !area.containsMouse
                }

                StyledText {
                    id: name

                    anchors.centerIn: parent
                    width: Math.min(implicitWidth, 230)
                    elide: Text.ElideMiddle
                    text: entry.modelData.name
                    color: area.containsMouse ? Theme.palette.labelOnAccent : Theme.palette.label
                    font.pointSize: Theme.font.size.smaller
                    font.weight: Font.Medium
                }
            }

            Item {
                id: thumb

                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                width: 48
                height: 48

                Image {
                    id: picture

                    anchors.fill: parent
                    visible: entry.modelData.image !== "" && status !== Image.Error
                    source: entry.modelData.image
                    sourceSize: Qt.size(96, 96)
                    fillMode: Image.PreserveAspectFit
                    asynchronous: true
                }

                IconImage {
                    anchors.fill: parent
                    visible: !picture.visible
                    implicitSize: 48
                    source: Shell.iconPath(entry.modelData.icon, entry.modelData.fallback || "text-x-generic")
                    asynchronous: true
                }
            }

            // Dragged out, it is the file: drop it in a window or a folder.
            Item {
                id: carried

                Drag.active: area.drag.active
                Drag.dragType: Drag.Automatic
                Drag.supportedActions: Qt.CopyAction | Qt.MoveAction | Qt.LinkAction
                Drag.mimeData: ({ "text/uri-list": entry.modelData.url.toString() })
                Drag.onDragFinished: root.closed()
            }

            MouseArea {
                id: area

                anchors.fill: thumb
                anchors.leftMargin: -270
                hoverEnabled: true
                drag.target: carried
                onClicked: {
                    root.folder.openItem(entry.modelData.path);
                    root.closed();
                }
            }
        }
    }
}
