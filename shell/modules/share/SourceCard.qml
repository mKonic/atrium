import QtQuick
import Atrium.Shell
import shell.components
import shell.services

// One screen or window to share: its picture (or its icon while there is
// none), then its name, ringed in the accent when picked.
Item {
    id: root

    property var source: ({})
    property bool chosen: false
    signal clicked
    signal doubleClicked

    readonly property bool screen: source.kind === "screen"

    width: 200
    height: 170

    Rectangle {
        id: frame

        width: parent.width
        height: 126
        radius: 12
        color: Theme.palette.tertiaryFill
        border.width: root.chosen ? 3 : 1
        border.color: root.chosen ? Theme.palette.accent : Theme.palette.separator

        Image {
            id: picture

            anchors.fill: parent
            anchors.margins: 6
            source: root.source.thumbnail ?? ""
            fillMode: Image.PreserveAspectFit
            asynchronous: true
            smooth: true
            mipmap: true
        }

        MaterialIcon {
            anchors.centerIn: parent
            visible: root.screen && picture.status !== Image.Ready
            text: "desktop_windows"
            font.pointSize: 34
            color: Theme.palette.secondaryLabel
        }

        IconImage {
            anchors.centerIn: parent
            visible: !root.screen && picture.status !== Image.Ready
            implicitSize: 48
            source: Icons.appIcon(root.source.appId ?? "")
        }
    }

    Row {
        anchors.top: frame.bottom
        anchors.topMargin: 8
        anchors.horizontalCenter: parent.horizontalCenter
        width: Math.min(implicitWidth, parent.width)
        spacing: 6

        IconImage {
            id: badge

            anchors.verticalCenter: parent.verticalCenter
            visible: !root.screen
            implicitSize: 16
            source: Icons.appIcon(root.source.appId ?? "")
        }

        StyledText {
            anchors.verticalCenter: parent.verticalCenter
            width: Math.min(implicitWidth, root.width - (badge.visible ? badge.width + 6 : 0))
            elide: Text.ElideRight
            text: root.screen ? root.source.name : (root.source.text || "Untitled")
            font.weight: Font.Medium
        }
    }

    StyledText {
        anchors.horizontalCenter: parent.horizontalCenter
        y: frame.height + 30
        width: Math.min(implicitWidth, parent.width)
        visible: root.screen && text !== ""
        elide: Text.ElideRight
        text: root.source.text ?? ""
        font.pointSize: Theme.font.size.small
        color: Theme.palette.secondaryLabel
    }

    MouseArea {
        anchors.fill: parent
        onClicked: root.clicked()
        onDoubleClicked: root.doubleClicked()
    }
}
