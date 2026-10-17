pragma ComponentBehavior: Bound

import QtQuick
import Atrium.Shell
import shell.components
import shell.services
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
    required property bool urgent   // a window of it asks for attention
    required property bool fixed    // the file manager: always first, never dragged off
    required property real badge     // the app's own count; -1: none
    required property real progress  // 0..1 of what it is doing; -1: none
    required property DockApps apps
    required property real magnification  // 1 at rest; the dock grows neighbours
    // Under the pointer (not pressed, no menu): the Dock shows its name.
    readonly property bool hovered: mouse.containsMouse && !mouse.pressed && !menuOpen
    required property real iconSize

    property bool launching: false
    property bool menuOpen: false
    property bool dropOver: false  // something dragged over it, to drop on it

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

    // For screen readers: the app, and whether (and how much) it's open.
    Accessible.role: Accessible.Button
    Accessible.name: name
    Accessible.description: !running ? "" : windowCount > 1 ? qsTr("Running, %1 windows").arg(windowCount) : qsTr("Running")
    Accessible.onPressAction: activate()

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

    // Asking for attention (a mention, a finished download, a window that
    // wanted the front): three hops, as macOS does for an informational
    // request, rather than hopping until it is looked at.
    signal attention
    onUrgentChanged: {
        if (urgent && !focused) {
            if (Theme.anim.on)
                attentionBounce.restart();
            attention();
        }
    }

    Timer {
        id: launchTimeout

        interval: 8000
        onTriggered: root.launching = false
    }

    // Under a drag: it takes what is dropped.
    Rectangle {
        anchors.centerIn: icon
        width: icon.width + 10
        height: icon.height + 10
        radius: Theme.rounding.normal
        color: Theme.palette.tertiaryFill
        border.width: 2
        border.color: Theme.palette.accent
        opacity: root.dropOver ? 1 : 0
        visible: opacity > 0

        Behavior on opacity {
            Anim {
                duration: Theme.anim.small
            }
        }
    }

    IconImage {
        id: icon

        readonly property real size: root.iconSize * root.magnification

        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: bounce.offset + (mouse.pressed ? -2 : 0)
        implicitSize: size
        largestSize: root.iconSize * 1.55  // the most the Dock magnifies
        scale: 0.6 + 0.4 * root.presence
        // A file URL is a picture the window sent itself.
        source: root.icon.startsWith("file:") ? root.icon : Shell.iconPath(root.icon, "application-x-executable")
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

    // The app's count (unread messages), as a Mac's red badge; quiet under
    // Do Not Disturb, as on KDE Plasma.
    Rectangle {
        readonly property bool shown: root.badge > 0 && !(Atrium.settings["notifications.dnd"] ?? false)

        anchors.right: icon.right
        anchors.top: icon.top
        anchors.rightMargin: -width * 0.25
        anchors.topMargin: -height * 0.2
        width: Math.max(height, badgeText.implicitWidth + 10)
        height: Math.round(root.iconSize * 0.4)
        radius: height / 2
        color: Theme.palette.red
        scale: shown ? 1 : 0
        visible: scale > 0

        Behavior on scale {
            Anim {
                duration: Theme.anim.small
                easing.bezierCurve: Theme.anim.emphasizedDecel
            }
        }

        StyledText {
            id: badgeText

            anchors.centerIn: parent
            text: root.badge > 99 ? "99+" : root.badge
            color: Theme.dark.label
            font.pointSize: Theme.font.size.smaller
            font.weight: Font.DemiBold
        }
    }

    // What it is busy with (a download), as a Mac's bar across the icon.
    Rectangle {
        anchors.horizontalCenter: icon.horizontalCenter
        anchors.bottom: icon.bottom
        anchors.bottomMargin: icon.height * 0.08
        width: icon.width * 0.8
        height: Math.max(4, Math.round(root.iconSize * 0.1))
        radius: height / 2
        color: Theme.material.thick
        border.width: 1
        border.color: Theme.palette.separator
        visible: root.progress >= 0

        Rectangle {
            anchors.left: parent.left
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            anchors.margins: 1
            width: Math.max(height, (parent.width - 2) * root.progress)
            radius: height / 2
            color: Theme.palette.accent

            Behavior on width {
                Anim {
                    duration: Theme.anim.small
                }
            }
        }
    }

    // Hops while the app is starting up.
    QtObject {
        id: bounce

        property real offset: 0
    }

    SequentialAnimation {
        id: attentionBounce

        loops: 3

        Anim {
            target: bounce
            property: "offset"
            to: root.iconSize * 0.5
            duration: 300
            easing.bezierCurve: Theme.anim.emphasizedDecel
        }
        Anim {
            target: bounce
            property: "offset"
            to: 0
            duration: 300
            easing.type: Easing.OutBounce
        }
    }

    SequentialAnimation {
        running: root.launching && !attentionBounce.running && Theme.anim.on  // the bounce stays still with reduced motion
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
        color: root.focused ? Theme.palette.accent : Theme.palette.secondaryLabel
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

    MouseArea {
        id: mouse

        property point pressAt
        property bool dragging: false

        anchors.fill: parent
        hoverEnabled: true
        acceptedButtons: Qt.LeftButton | Qt.RightButton | Qt.MiddleButton
        preventStealing: true
        onPressed: event => {
            pressAt = Qt.point(event.x, event.y);
            dragging = false;
        }
        onPositionChanged: event => {
            if (!(event.buttons & Qt.LeftButton))
                return;
            const at = mapToItem(null, event.x, event.y);
            if (!dragging && !root.fixed && Math.abs(event.x - pressAt.x) + Math.abs(event.y - pressAt.y) > 8) {
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
            else if (event.button === Qt.MiddleButton)
                root.apps.launch(root.appId);  // a new window, as on KDE Plasma
            else
                root.activate();
        }

        // A notch of the wheel steps through its windows (KDE Plasma's
        // "cycle through tasks", for this app only).
        property real wheelRest: 0
        onWheel: event => {
            wheelRest += event.angleDelta.y || -event.angleDelta.x;
            while (Math.abs(wheelRest) >= 120) {
                root.apps.cycle(root.appId, wheelRest < 0 ? 1 : -1);
                wheelRest -= wheelRest > 0 ? 120 : -120;
            }
        }
    }
}
