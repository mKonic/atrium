pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Effects
import Atrium.Shell
import shell.components
import shell.services

// One file on the desktop: its icon (a thumbnail for pictures, the app's
// own icon for launchers) and name. Drag it (with the rest selected) to
// move it, onto a folder or app, or out to hand the files to an app.
Item {
    id: root

    required property string path
    required property string name      // what it shows: a launcher's own name
    required property string fileName  // what it is called on disk
    required property string iconName
    required property bool isDir
    required property bool isImage
    required property bool isLink
    required property bool isHidden
    required property bool selected
    property bool renaming: false
    property bool dropTarget: false  // a drag over it would drop in
    property int iconSize: 54
    // What a drag carries, set when pressed.
    property string uris: ""

    signal pressed(var event, bool right)
    signal released(var event, bool onName)
    signal doubleClicked
    signal renamed(string name)
    signal renameDone  // Enter or Escape

    opacity: isHidden ? 0.6 : 1

    Rectangle {
        id: tile

        anchors.horizontalCenter: parent.horizontalCenter
        y: 2
        width: root.iconSize + 14
        height: root.iconSize + 14
        radius: Theme.rounding.small
        color: root.dropTarget ? Theme.palette.accentFill
             : root.selected ? Theme.palette.fill
             : mouse.containsMouse ? Theme.palette.quaternaryFill : "transparent"
        border.width: root.dropTarget ? 2 : 0
        border.color: Theme.palette.accent
    }

    IconImage {
        anchors.centerIn: tile
        implicitSize: root.iconSize
        visible: !root.isImage
        source: root.isImage ? "" : Shell.iconPath(root.iconName, "text-x-generic")
        asynchronous: true
    }

    // Pictures get a white frame, like a print.
    Rectangle {
        anchors.centerIn: tile
        visible: root.isImage && thumb.status === Image.Ready
        width: thumb.paintedWidth + 6
        height: thumb.paintedHeight + 6
        radius: 3
        color: Theme.dark.label

        layer.enabled: true
        layer.effect: MultiEffect {
            shadowEnabled: true
            shadowColor: Theme.dark.shadow
            shadowBlur: 0.4
            shadowVerticalOffset: 1
        }
    }

    Image {
        id: thumb

        anchors.centerIn: tile
        width: root.iconSize + 4
        height: root.iconSize + 4
        visible: root.isImage
        source: root.isImage ? `file://${root.path}` : ""
        sourceSize.width: (root.iconSize + 4) * 2
        sourceSize.height: (root.iconSize + 4) * 2
        fillMode: Image.PreserveAspectFit
        asynchronous: true
        smooth: true
        mipmap: true
    }

    // A link: the small arrow Finder and Explorer put on a shortcut.
    MaterialIcon {
        anchors.left: tile.left
        anchors.bottom: tile.bottom
        anchors.margins: 4
        visible: root.isLink
        text: "shortcut"
        font.pointSize: Theme.font.size.small
        color: Theme.dark.label
        style: Text.Outline
        styleColor: Theme.dark.shadow
    }

    Rectangle {
        id: nameArea

        anchors.horizontalCenter: parent.horizontalCenter
        anchors.top: tile.bottom
        anchors.topMargin: 4
        width: Math.min(label.implicitWidth, label.maxWidth) + 10
        height: label.implicitHeight + 4
        radius: 6
        color: root.selected ? Theme.palette.accent : "transparent"
        visible: !root.renaming

        StyledText {
            id: label

            readonly property int maxWidth: root.width - 8

            anchors.centerIn: parent
            width: Math.min(implicitWidth, maxWidth)
            text: root.name
            horizontalAlignment: Text.AlignHCenter
            // Whole words to the next line; only a word too long for a line breaks.
            wrapMode: Text.WrapAtWordBoundaryOrAnywhere
            maximumLineCount: root.selected ? 3 : 2
            elide: Text.ElideRight
            font.pointSize: Theme.font.size.small
            font.weight: Font.Medium
            color: root.selected ? Theme.palette.labelOnAccent : Theme.dark.label

            // Readable on any wallpaper: a soft shadow, as macOS gives desktop labels.
            layer.enabled: !root.selected
            layer.effect: MultiEffect {
                shadowEnabled: true
                shadowColor: Theme.dark.shadow
                shadowBlur: 0.45
                shadowVerticalOffset: 1
                shadowHorizontalOffset: 0
            }
        }
    }

    // Rename in place: the name itself turns editable, same text, same
    // spot. Enter or a click elsewhere keeps the new name, Escape the old.
    Rectangle {
        z: 1  // over the icon's mouse area: a click places the cursor
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.top: tile.bottom
        anchors.topMargin: 4
        width: label.maxWidth
        height: input.implicitHeight + 6
        radius: 6
        visible: root.renaming
        color: Theme.palette.windowBackground
        border.width: 1
        border.color: Theme.palette.accent

        TextInput {
            id: input

            anchors.fill: parent
            anchors.margins: 3
            horizontalAlignment: TextInput.AlignHCenter
            color: Theme.palette.label
            font.family: Theme.font.sans
            font.pointSize: Theme.font.size.small
            font.weight: Font.Medium
            selectByMouse: true
            selectionColor: Theme.palette.accent
            selectedTextColor: Theme.palette.labelOnAccent
            cursorVisible: true
            clip: true
            onAccepted: root.renameDone()
            Keys.onEscapePressed: {
                text = root.fileName;
                root.renameDone();
            }
        }
    }

    onRenamingChanged: {
        if (!renaming) {
            root.renamed(input.text);
            return;
        }
        input.text = fileName;
        input.forceActiveFocus();
        const dot = fileName.lastIndexOf(".");
        input.select(0, dot > 0 && !isDir ? dot : fileName.length);
    }

    MouseArea {
        id: mouse

        anchors.fill: parent
        hoverEnabled: true
        acceptedButtons: Qt.LeftButton | Qt.RightButton
        drag.target: dragProxy
        drag.threshold: Application.styleHints.startDragDistance
        onPressed: event => root.pressed(event, event.button === Qt.RightButton)
        onReleased: event => {
            if (event.button === Qt.LeftButton && !drag.active)
                root.released(event, nameArea.contains(mapToItem(nameArea, event.x, event.y)));
        }
        onDoubleClicked: event => {
            if (event.button === Qt.LeftButton)
                root.doubleClicked();
        }
    }

    // What leaves the desktop when the icon is dragged: the files' URLs, so
    // any app that takes dropped files takes them.
    Item {
        id: dragProxy

        width: 1
        height: 1
        Drag.active: mouse.drag.active
        Drag.dragType: Drag.Automatic
        Drag.supportedActions: Qt.CopyAction | Qt.MoveAction | Qt.LinkAction
        Drag.mimeData: ({ "text/uri-list": root.uris })
        Drag.imageSource: root.isImage ? `file://${root.path}` : Shell.iconPath(root.iconName, "text-x-generic")
    }
}
