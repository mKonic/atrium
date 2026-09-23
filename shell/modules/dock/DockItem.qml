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
    required property var windows       // its open windows, most recent first
    required property bool pinned
    required property real magnification  // 1 at rest; the dock grows neighbours
    required property real iconSize

    readonly property DesktopEntry entry: DesktopEntries.heuristicLookup(appId)
    readonly property bool running: windows.length > 0
    readonly property bool focused: windows.some(w => w.focused)
    readonly property string name: entry?.name ?? appId
    property bool launching: false
    property bool menuOpen: false

    signal menuRequested(Item item)

    implicitWidth: iconSize * magnification
    implicitHeight: iconSize

    Behavior on implicitWidth {
        Anim {
            duration: Theme.anim.small
        }
    }

    function activate(): void {
        if (!running) {
            if (!entry)
                return;
            entry.execute();
            launching = true;
            launchTimeout.restart();
            return;
        }
        // Already in front: step to its next window, as Cmd+` does.
        const target = focused && windows.length > 1 ? windows[windows.length - 1] : windows[0];
        Atrium.focusWindow(target.id);
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
        source: Icons.appIcon(root.appId)
        asynchronous: true
        smooth: true
        mipmap: true

        Behavior on implicitSize {
            Anim {
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

        anchors.fill: parent
        hoverEnabled: true
        acceptedButtons: Qt.LeftButton | Qt.RightButton
        onClicked: event => {
            if (event.button === Qt.RightButton)
                root.menuRequested(root);
            else
                root.activate();
        }
    }
}
