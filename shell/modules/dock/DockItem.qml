pragma ComponentBehavior: Bound

import QtQuick
import Quickshell
import Quickshell.Widgets
import qs.components
import qs.services
import Atrium

// One app in the Dock: its icon, a dot while it runs, its name on hover.
// Click focuses it (cycling its windows) or launches it; right-click opens
// its menu.
Item {
    id: root

    required property string appId      // desktop entry id
    required property string name
    required property string icon
    required property bool pinned
    required property bool running
    required property bool focused
    required property int windowCount
    required property bool leaving  // just closed: shrinking away
    required property DockApps apps
    required property real magnification  // 1 at rest; the dock grows neighbours
    required property real iconSize

    property bool launching: false
    property bool menuOpen: false

    property bool lifted: false  // being dragged to a new place
    // 0 → 1 as it arrives, back to 0 as it leaves: the Dock opens and closes
    // around it instead of snapping.
    property real presence: 0
    Component.onCompleted: presence = leaving ? 0 : 1
    onLeavingChanged: presence = leaving ? 0 : 1

    Behavior on presence {
        Anim {
            duration: Theme.anim.small
            easing.bezierCurve: Theme.anim.emphasizedDecel
        }
    }
    signal menuRequested(Item item)
    // A press that moved: the Dock rearranges. Points are in the Dock
    // window's coordinates.
    signal dragStarted(Item item, point at)
    signal dragMoved(point at)
    signal dragEnded(point at)

    // Magnification changes with every pointer move, so follow it smoothly
    // rather than restarting an animation each time.
    // The gap to its neighbours is part of it, so it closes with it too.
    readonly property real gap: 8
    implicitWidth: (iconSize * magnification + gap) * presence
    implicitHeight: iconSize
    opacity: (lifted ? 0.25 : 1) * presence
    enabled: !leaving

    Behavior on implicitWidth {
        enabled: root.presence === 1
        SmoothedAnimation {
            velocity: -1
            duration: Theme.anim.small
        }
    }

    function activate(): void {
        if (apps.activate(appId)) {
            launching = true;
            launchTimeout.restart();
        }
    }

    onRunningChanged: {
        if (running)
            launching = false;
    }

    Timer {
        id: launchTimeout

        interval: 8000
        onTriggered: root.launching = false
    }

    IconImage {
        id: icon

        readonly property real size: root.iconSize * root.magnification

        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: bounce.offset + (mouse.pressed ? -2 : 0)
        implicitSize: size
        scale: 0.6 + 0.4 * root.presence
        // A file URL is a picture the window sent itself.
        source: root.icon.startsWith("file:") ? root.icon : Quickshell.iconPath(root.icon, "application-x-executable")
        asynchronous: true
        smooth: true
        mipmap: true

        Behavior on implicitSize {
            SmoothedAnimation {
                velocity: -1
                duration: Theme.anim.small
            }
        }
    }

    // Hops while the app is starting up.
    QtObject {
        id: bounce

        property real offset: 0
    }

    SequentialAnimation {
        running: root.launching
        loops: Animation.Infinite
        alwaysRunToEnd: true

        Anim {
            target: bounce
            property: "offset"
            to: root.iconSize * 0.35
            duration: 280
            easing.bezierCurve: Theme.anim.emphasizedDecel
        }
        Anim {
            target: bounce
            property: "offset"
            to: 0
            duration: 280
            easing.type: Easing.OutBounce
        }
    }

    // Running dot; wider for the app in front.
    Rectangle {
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.top: parent.bottom
        anchors.topMargin: 3
        width: root.focused ? 12 : 4
        height: 4
        radius: 2
        color: root.focused ? Theme.palette.m3Primary : Theme.palette.m3OnSurfaceVariant
        opacity: root.running ? 1 : 0

        Behavior on width {
            Anim {
                duration: Theme.anim.small
            }
        }

        Behavior on opacity {
            Anim {
                duration: Theme.anim.small
            }
        }
    }

    // Name above the icon while hovered.
    Rectangle {
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: icon.top
        anchors.bottomMargin: 10
        width: label.implicitWidth + Theme.padding.larger * 2
        height: label.implicitHeight + Theme.padding.small * 2
        radius: Theme.rounding.full
        color: Theme.palette.m3SurfaceContainerHigh
        border.width: 1
        border.color: Theme.alpha(Theme.palette.m3OutlineVariant, 0.6)
        opacity: mouse.containsMouse && !mouse.pressed && !root.menuOpen ? 1 : 0
        visible: opacity > 0

        Behavior on opacity {
            Anim {
                duration: Theme.anim.small
            }
        }

        StyledText {
            id: label

            anchors.centerIn: parent
            text: root.name
            font.pointSize: Theme.font.size.smaller
            font.weight: Font.Medium
        }
    }

    MouseArea {
        id: mouse

        property point pressAt
        property bool dragging: false

        anchors.fill: parent
        hoverEnabled: true
        acceptedButtons: Qt.LeftButton | Qt.RightButton
        preventStealing: true
        onPressed: event => {
            pressAt = Qt.point(event.x, event.y);
            dragging = false;
        }
        onPositionChanged: event => {
            if (!(event.buttons & Qt.LeftButton))
                return;
            const at = mapToItem(null, event.x, event.y);
            if (!dragging && Math.abs(event.x - pressAt.x) + Math.abs(event.y - pressAt.y) > 8) {
                dragging = true;
                root.dragStarted(root, at);
            }
            if (dragging)
                root.dragMoved(at);
        }
        onReleased: event => {
            if (dragging)
                root.dragEnded(mapToItem(null, event.x, event.y));
        }
        onClicked: event => {
            if (dragging)
                return;
            if (event.button === Qt.RightButton)
                root.menuRequested(root);
            else
                root.activate();
        }
    }
}
