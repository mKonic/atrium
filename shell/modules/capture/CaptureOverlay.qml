pragma ComponentBehavior: Bound

import QtQuick
import Atrium.Shell
import shell.components
import shell.services
import Atrium

// One screen, frozen, to pick from: drag out a region, click a window or a
// screen, or (for the portal) a colour. Space switches region and window,
// as on a Mac; Escape leaves.
PanelWindow {
    id: root

    readonly property string name: screen?.name ?? ""
    readonly property bool focused: (Atrium.focusedOutput?.name ?? name) === name
    property point from
    property point to
    property bool dragging: false
    property var hovered: ({})  // the window under the pointer
    property bool inside: false
    property string color

    anchors {
        top: true
        bottom: true
        left: true
        right: true
    }
    exclusiveZone: -1
    color: "black"
    WlrLayershell.layer: WlrLayer.Overlay
    WlrLayershell.namespace: "atrium-screenshot"
    WlrLayershell.keyboardFocus: focused ? WlrKeyboardFocus.Exclusive : WlrKeyboardFocus.None

    Image {
        anchors.fill: parent
        source: Capture.frozen[root.name] ?? ""
        smooth: true
    }

    // The dim, with the selection or what's hovered left clear.
    Item {
        id: dim

        readonly property rect clear: Capture.kind === "region" && root.dragging
                ? Qt.rect(Math.min(root.from.x, root.to.x), Math.min(root.from.y, root.to.y),
                          Math.abs(root.to.x - root.from.x), Math.abs(root.to.y - root.from.y))
            : Capture.kind === "window" && root.hovered.identifier
                ? Qt.rect(root.hovered.x, root.hovered.y, root.hovered.width, root.hovered.height)
            : Capture.kind === "screen" && root.inside ? Qt.rect(0, 0, root.width, root.height)
            : Qt.rect(0, 0, 0, 0)
        readonly property color shade: Qt.rgba(0, 0, 0, 0.35)

        anchors.fill: parent

        Rectangle {
            width: parent.width
            height: dim.clear.height > 0 ? dim.clear.y : parent.height
            color: dim.shade
        }

        Rectangle {
            visible: dim.clear.height > 0
            y: dim.clear.y + dim.clear.height
            width: parent.width
            height: parent.height - y
            color: dim.shade
        }

        Rectangle {
            visible: dim.clear.height > 0
            y: dim.clear.y
            width: dim.clear.x
            height: dim.clear.height
            color: dim.shade
        }

        Rectangle {
            visible: dim.clear.height > 0
            x: dim.clear.x + dim.clear.width
            y: dim.clear.y
            width: parent.width - x
            height: dim.clear.height
            color: dim.shade
        }

        // A window or screen about to be taken: tinted in the accent.
        Rectangle {
            visible: Capture.kind !== "region" && dim.clear.width > 0
            x: dim.clear.x
            y: dim.clear.y
            width: dim.clear.width
            height: dim.clear.height
            color: Theme.alpha(Theme.palette.m3Primary, 0.22)
            border.width: 2
            border.color: Theme.palette.m3Primary
        }

        Rectangle {
            visible: Capture.kind === "region" && root.dragging
            x: dim.clear.x - 1
            y: dim.clear.y - 1
            width: dim.clear.width + 2
            height: dim.clear.height + 2
            color: "transparent"
            border.width: 1
            border.color: "white"
        }
    }

    // Its size, beside the pointer while dragging; a colour's swatch.
    Rectangle {
        visible: (Capture.kind === "region" && root.dragging) || (Capture.kind === "color" && root.inside)
        x: Math.min(root.to.x + 14, root.width - width - 4)
        y: Math.min(root.to.y + 14, root.height - height - 4)
        width: sizeRow.implicitWidth + 16
        height: 26
        radius: 8
        color: Qt.rgba(0, 0, 0, 0.7)

        Row {
            id: sizeRow

            anchors.centerIn: parent
            spacing: 6

            Rectangle {
                anchors.verticalCenter: parent.verticalCenter
                visible: Capture.kind === "color"
                width: 14
                height: 14
                radius: 4
                color: root.color || "transparent"
                border.width: 1
                border.color: Qt.rgba(1, 1, 1, 0.6)
            }

            StyledText {
                text: Capture.kind === "color" ? root.color : `${Math.round(dim.clear.width)} × ${Math.round(dim.clear.height)}`
                color: "white"
                font.pointSize: Theme.font.size.small
                font.family: Theme.font.mono
            }
        }
    }

    MouseArea {
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Capture.kind === "region" || Capture.kind === "color" ? Qt.CrossCursor : Qt.PointingHandCursor

        onEntered: root.inside = true
        onExited: {
            root.inside = false;
            root.hovered = {};
        }
        onPressed: mouse => {
            root.from = Qt.point(mouse.x, mouse.y);
            root.to = root.from;
            root.dragging = Capture.kind === "region";
        }
        onPositionChanged: mouse => {
            root.to = Qt.point(mouse.x, mouse.y);
            if (Capture.kind === "window")
                root.hovered = Capture.windowAt(root.name, mouse.x, mouse.y);
            else if (Capture.kind === "color")
                root.color = Capture.colorAt(root.name, mouse.x, mouse.y);
        }
        onReleased: mouse => {
            if (Capture.kind === "region" && root.dragging) {
                root.dragging = false;
                Capture.takeRegion(root.name, root.from.x, root.from.y, mouse.x, mouse.y);
            }
        }
        onClicked: mouse => {
            if (Capture.kind === "window" && root.hovered.identifier)
                Capture.takeWindow(root.hovered.identifier);
            else if (Capture.kind === "screen")
                Capture.takeScreen(root.name);
            else if (Capture.kind === "color")
                Capture.pickColor(root.name, mouse.x, mouse.y);
        }
    }

    Item {
        anchors.fill: parent
        focus: root.focused
        Keys.onEscapePressed: Capture.cancel()
        Keys.onSpacePressed: {
            if (Capture.kind === "region" || Capture.kind === "window")
                Capture.kind = Capture.kind === "region" ? "window" : "region";
        }
    }

    CaptureToolbar {
        visible: Capture.toolbar && root.focused
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 110
    }
}
