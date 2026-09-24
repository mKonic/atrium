import QtQuick
import QtQuick.Effects
import Atrium.Shell
import shell.components
import shell.services
import Atrium

// The shot, small, in the screen's corner for a few seconds as on a Mac:
// click it to mark it up; it's already saved and on the clipboard.
PanelWindow {
    id: root

    signal open

    readonly property bool shown: Capture.lastFile !== "" && !Capture.forPortal && !opened
    property bool opened: false

    visible: shown
    screen: Shell.screen(Atrium.focusedOutput?.name)
    anchors {
        bottom: true
        right: true
    }
    margins {
        bottom: 24
        right: 24
    }
    implicitWidth: 240
    implicitHeight: card.height + 24
    exclusiveZone: -1
    color: "transparent"
    WlrLayershell.layer: WlrLayer.Overlay
    WlrLayershell.namespace: "atrium-screenshot-thumbnail"

    // Gone after a while unless the pointer is on it.
    Timer {
        running: root.shown && !area.containsMouse
        interval: 6000
        onTriggered: Capture.cancel()
    }

    Rectangle {
        id: card

        anchors.bottom: parent.bottom
        anchors.right: parent.right
        width: 216
        height: Math.max(60, Math.min(200, width * (picture.implicitHeight / Math.max(1, picture.implicitWidth))))
        radius: 12
        color: Theme.palette.m3SurfaceContainerHigh
        border.width: 3
        border.color: "white"

        layer.enabled: true
        layer.effect: MultiEffect {
            shadowEnabled: true
            shadowColor: Qt.rgba(0, 0, 0, 0.45)
            shadowBlur: 1
            shadowVerticalOffset: 6
        }

        Image {
            id: picture

            anchors.fill: parent
            anchors.margins: 3
            source: Capture.lastUrl
            fillMode: Image.PreserveAspectFit
            asynchronous: true
            mipmap: true
            cache: false
        }

        MouseArea {
            id: area

            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: {
                root.opened = true;
                root.open();
            }
        }

        // Throw it away without opening it.
        Rectangle {
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.margins: -8
            visible: area.containsMouse || bin.containsMouse
            width: 26
            height: 26
            radius: 13
            color: Theme.palette.m3SurfaceContainerHigh
            border.width: 1
            border.color: Theme.alpha(Theme.palette.m3Outline, 0.3)

            MaterialIcon {
                anchors.centerIn: parent
                text: "delete"
                font.pointSize: Theme.font.size.small
            }

            MouseArea {
                id: bin

                anchors.fill: parent
                hoverEnabled: true
                onClicked: Capture.deleteLast()
            }
        }
    }
}
