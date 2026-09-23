pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Effects
import Quickshell
import Quickshell.Io
import Quickshell.Widgets
import qs.components
import qs.services

// One file on the desktop: its icon (a thumbnail for pictures, the app's
// own icon for launchers) and name. Drag it out to hand the file to an app.
Item {
    id: root

    required property string path
    required property string name
    required property string suffix
    required property bool isDir
    required property bool selected
    property bool renaming: false

    signal clicked(var event)
    signal doubleClicked
    signal renamed(string name)

    readonly property bool isImage: ["png", "jpg", "jpeg", "webp", "gif", "bmp", "svg"].includes(suffix.toLowerCase())
    readonly property bool isLauncher: suffix === "desktop"

    // Launchers name and draw themselves.
    property string launcherName: ""
    property string launcherIcon: ""

    width: 120
    height: 104

    FileView {
        path: root.isLauncher ? root.path : ""
        onLoaded: {
            const t = text();
            root.launcherName = (t.match(/^Name=(.*)$/m) ?? [])[1] ?? "";
            root.launcherIcon = (t.match(/^Icon=(.*)$/m) ?? [])[1] ?? "";
        }
    }

    function iconName(): string {
        if (isDir)
            return "folder";
        if (isLauncher)
            return launcherIcon || "application-x-executable";
        const s = suffix.toLowerCase();
        const byType = {
            pdf: "application-pdf",
            zip: "package-x-generic", tar: "package-x-generic", gz: "package-x-generic", xz: "package-x-generic",
            zst: "package-x-generic", "7z": "package-x-generic", rar: "package-x-generic",
            mp3: "audio-x-generic", flac: "audio-x-generic", ogg: "audio-x-generic", wav: "audio-x-generic",
            mp4: "video-x-generic", mkv: "video-x-generic", webm: "video-x-generic", mov: "video-x-generic",
            sh: "text-x-script", py: "text-x-script", fish: "text-x-script",
            txt: "text-x-generic", md: "text-x-generic", json: "text-x-generic",
            html: "text-html", doc: "x-office-document", docx: "x-office-document", odt: "x-office-document",
            xls: "x-office-spreadsheet", xlsx: "x-office-spreadsheet", ods: "x-office-spreadsheet",
            iso: "media-optical", img: "media-optical"
        };
        return byType[s] ?? "text-x-generic";
    }

    Rectangle {
        id: tile

        anchors.horizontalCenter: parent.horizontalCenter
        y: 2
        width: 68
        height: 68
        radius: Theme.rounding.small
        color: root.selected ? Theme.alpha(Theme.palette.m3OnSurface, 0.16) : "transparent"
    }

    IconImage {
        anchors.centerIn: tile
        implicitSize: 54
        visible: !root.isImage
        source: root.isImage ? "" : Quickshell.iconPath(root.iconName(), "text-x-generic")
        asynchronous: true
    }

    // Pictures get a white frame, like a print.
    Rectangle {
        anchors.centerIn: tile
        visible: root.isImage && thumb.status === Image.Ready
        width: thumb.paintedWidth + 6
        height: thumb.paintedHeight + 6
        radius: 3
        color: "#f4f4f4"

        layer.enabled: true
        layer.effect: MultiEffect {
            shadowEnabled: true
            shadowColor: Qt.rgba(0, 0, 0, 0.5)
            shadowBlur: 0.4
            shadowVerticalOffset: 1
        }
    }

    Image {
        id: thumb

        anchors.centerIn: tile
        width: 58
        height: 58
        visible: root.isImage
        source: root.isImage ? `file://${root.path}` : ""
        sourceSize.width: 116
        sourceSize.height: 116
        fillMode: Image.PreserveAspectFit
        asynchronous: true
        smooth: true
        mipmap: true
    }

    Rectangle {
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.top: tile.bottom
        anchors.topMargin: 4
        width: Math.min(label.implicitWidth, label.maxWidth) + 10
        height: label.implicitHeight + 4
        radius: 6
        color: root.selected ? Theme.palette.m3Primary : "transparent"
        visible: !root.renaming

        StyledText {
            id: label

            readonly property int maxWidth: root.width - 8

            anchors.centerIn: parent
            width: Math.min(implicitWidth, maxWidth)
            text: root.isLauncher && root.launcherName ? root.launcherName : root.name
            horizontalAlignment: Text.AlignHCenter
            // Whole words to the next line; only a word too long for a line breaks.
            wrapMode: Text.WrapAtWordBoundaryOrAnywhere
            maximumLineCount: root.selected ? 3 : 2
            elide: Text.ElideRight
            font.pointSize: Theme.font.size.small
            font.weight: Font.Medium
            color: root.selected ? Theme.palette.m3OnPrimary : "white"

            // Readable on any wallpaper: a soft shadow, as macOS gives desktop labels.
            layer.enabled: !root.selected
            layer.effect: MultiEffect {
                shadowEnabled: true
                shadowColor: Qt.rgba(0, 0, 0, 0.85)
                shadowBlur: 0.45
                shadowVerticalOffset: 1
                shadowHorizontalOffset: 0
            }
        }
    }

    // Rename in place: the label itself turns editable, same text, same spot.
    Item {
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.top: tile.bottom
        anchors.topMargin: 6
        width: label.maxWidth
        height: input.implicitHeight
        visible: root.renaming

        TextInput {
            id: input

            anchors.fill: parent
            horizontalAlignment: TextInput.AlignHCenter
            color: "white"
            font.family: Theme.font.sans
            font.pointSize: Theme.font.size.small
            font.weight: Font.Medium
            selectByMouse: true
            selectionColor: Theme.palette.m3Primary
            selectedTextColor: Theme.palette.m3OnPrimary
            cursorVisible: true
            clip: true
            onAccepted: root.renamed(text)
            Keys.onEscapePressed: root.renamed(root.name)

            layer.enabled: true
            layer.effect: MultiEffect {
                shadowEnabled: true
                shadowColor: Qt.rgba(0, 0, 0, 0.85)
                shadowBlur: 0.45
                shadowVerticalOffset: 1
            }
        }
    }

    onRenamingChanged: {
        if (!renaming)
            return;
        input.text = name;
        input.forceActiveFocus();
        const dot = name.lastIndexOf(".");
        input.select(0, dot > 0 && !isDir ? dot : name.length);
    }

    MouseArea {
        id: mouse

        anchors.fill: parent
        acceptedButtons: Qt.LeftButton | Qt.RightButton
        drag.target: dragProxy
        onClicked: event => root.clicked(event)
        onDoubleClicked: event => {
            if (event.button === Qt.LeftButton)
                root.doubleClicked();
        }
    }

    // What leaves the desktop when the icon is dragged: the file's URL, so
    // any app that takes dropped files takes it.
    Item {
        id: dragProxy

        width: 1
        height: 1
        Drag.active: mouse.drag.active
        Drag.dragType: Drag.Automatic
        Drag.supportedActions: Qt.CopyAction | Qt.MoveAction | Qt.LinkAction
        Drag.mimeData: ({ "text/uri-list": `file://${root.path}\r\n` })
        Drag.imageSource: root.isImage ? `file://${root.path}` : Quickshell.iconPath(root.iconName(), "text-x-generic")
    }
}
