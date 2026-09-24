pragma ComponentBehavior: Bound

import QtQuick
import Atrium.Shell
import shell.components
import shell.services
import Atrium

// The Dock: pinned apps, then running ones that aren't pinned, on a frosted
// shelf along the bottom. The window is taller than the shelf so names and
// menus have room above it; only the shelf (and an open menu) take input.
PanelWindow {
    id: dock

    readonly property real iconSize: 48
    readonly property real shelfPadding: 8
    readonly property real shelfHeight: iconSize + shelfPadding * 2 + 6
    readonly property real gap: 8  // under the shelf
    readonly property bool magnify: Atrium.settings["dock.magnify"] ?? false

    DockApps {
        id: dockApps

        entries: DesktopEntries
    }

    property real pointerX: -1  // over the shelf, for magnification

    // Dragging an app to a new place among the pins, or off the Dock.
    property Item dragItem: null
    property point dragAt
    readonly property bool dragRemoves: dragItem !== null && dragItem.pinned && dragAt.y < shelf.y - 50
    // Where among the pinned apps it would land; -1 outside them.
    readonly property int dropIndex: {
        if (!dragItem || dragRemoves)
            return -1;
        let index = 0, lastPinnedEdge = -1;
        for (let i = 0; i < apps.count; ++i) {
            const it = apps.itemAt(i);
            if (!it || !it.pinned)
                continue;
            const box = it.mapToItem(dock.contentItem, 0, 0, it.width, it.height);
            lastPinnedEdge = box.x + box.width;
            if (it !== dragItem && box.x + box.width / 2 < dragAt.x)
                ++index;
        }
        // A running app joins the pins only when dropped among them.
        if (!dragItem.pinned && dragAt.x > lastPinnedEdge + dock.iconSize / 2)
            return -1;
        return index;
    }

    function endDrag(): void {
        const item = dragItem;
        if (!item)
            return;
        // Read where it lands before letting go: both follow dragItem.
        const removes = dragRemoves;
        const index = dropIndex;
        dragItem = null;
        item.lifted = false;
        if (removes)
            dockApps.setPinned(item.appId, false);
        else if (index >= 0)
            dockApps.placePin(item.appId, index);
    }
    property var menuItem: null

    // Out of sight over a fullscreen app or a tiled space (or always, with
    // dock.autohide) until the pointer reaches the bottom edge.
    readonly property bool fullscreen: output.fullscreen

    OutputState {
        id: output

        name: dock.screen?.name ?? ""
    }
    readonly property bool autohide: Atrium.settings["dock.autohide"] ?? false
    // Nothing pinned and nothing open: no Dock at all, not an empty shelf.
    readonly property bool empty: dockApps.count === 0
    // One Dock, on DockPlace.screen; the other screens keep only the edge
    // that brings it over.
    readonly property bool home: (Atrium.settings["dock.every_screen"] ?? false) || DockPlace.screen === (screen?.name ?? "")
    readonly property bool hides: fullscreen || output.tiled || autohide || empty || !home
    property bool revealed: !hides

    onHidesChanged: revealed = !hides

    WlrLayershell.layer: fullscreen ? WlrLayer.Overlay : WlrLayer.Top

    HoverHandler {
        id: hover

        onHoveredChanged: {
            if (!dock.home) {
                // Resting at the bottom of this screen moves the Dock here.
                claimTimer.running = hovered;
                return;
            }
            if (hovered)
                dock.revealed = !dock.empty;
            else if (dock.hides && !dock.menuItem)
                hideTimer.restart();
        }
    }

    Timer {
        id: claimTimer

        interval: 500
        onTriggered: DockPlace.screen = dock.screen?.name ?? ""
    }

    onHomeChanged: if (home && hover.hovered) revealed = !empty

    Timer {
        id: hideTimer

        interval: 450
        onTriggered: dock.revealed = !dock.empty && (!dock.hides || hover.hovered || dock.menuItem !== null)
    }

    onMenuItemChanged: {
        if (!menuItem && hides && !hover.hovered)
            hideTimer.restart();
    }

    Item {
        id: edge

        anchors.bottom: parent.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        height: 2
    }

    // Where the shelf rests plus the gap under it down to the screen edge,
    // so the pointer that revealed the Dock is inside it at once (the shelf
    // itself is still sliding up from below then).
    Item {
        id: reach

        anchors.bottom: parent.bottom
        anchors.horizontalCenter: parent.horizontalCenter
        width: shelf.width
        height: dock.shelfHeight + dock.gap
    }

    anchors.bottom: true
    // Away from its screen, the whole bottom edge brings it over.
    implicitWidth: home ? Math.max(shelf.width + 40, menu.width + 40) : (screen?.width ?? 0)
    implicitHeight: shelfHeight + gap + 150
    exclusiveZone: autohide || empty || output.tiled || !home ? 0 : shelfHeight + gap
    color: "transparent"
    WlrLayershell.namespace: "atrium-dock"

    mask: Region {
        item: dock.revealed ? reach : edge

        Region {
            item: dock.menuItem ? menu : null
        }
    }

    Rectangle {
        id: shelf

        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: dock.revealed ? dock.gap : -(dock.shelfHeight + 4)
        // The icons' own gaps reach 4px past each end; the padding covers the rest.
        width: Math.max(0, row.width - 8) + dock.shelfPadding * 2
        height: dock.shelfHeight
        radius: 22
        color: Theme.panel(Theme.palette.m3SurfaceContainer, 0.62)

        GlassRim {}


        Behavior on color {
            CAnim {}
        }
        border.width: 1
        border.color: Theme.alpha(Theme.palette.m3Outline, 0.18)

        // No animation of its own: it follows the icons, which grow and
        // shrink smoothly as apps come and go.

        Behavior on anchors.bottomMargin {
            Anim {
                duration: Theme.anim.small
                easing.bezierCurve: Theme.anim.emphasizedDecel
            }
        }

        MouseArea {
            anchors.fill: parent
            hoverEnabled: true
            acceptedButtons: Qt.NoButton
            onPositionChanged: event => dock.pointerX = event.x
            onExited: dock.pointerX = -1
        }

        Row {
            id: row

            anchors.horizontalCenter: parent.horizontalCenter
            anchors.bottom: parent.bottom
            anchors.bottomMargin: dock.shelfPadding + 6
            spacing: 0  // each icon carries its own gap (DockItem.gap)
            // No move transition: neighbours glide because the icons beside
            // them grow and shrink; a transition here would restart on every
            // frame of that and stutter.

            Repeater {
                id: apps

                model: dockApps

                DockItem {
                    id: item

                    required property int index
                    required property bool divider

                    apps: dockApps
                    iconSize: dock.iconSize
                    menuOpen: dock.menuItem === item
                    magnification: {
                        if (!dock.magnify || dock.pointerX < 0)
                            return 1;
                        const center = x + width / 2 + row.x;
                        const d = Math.abs(dock.pointerX - center) / (dock.iconSize * 2.2);
                        return 1 + 0.55 * Math.max(0, Math.cos(Math.min(d, 1) * Math.PI / 2));
                    }
                    onMenuRequested: item => dock.menuItem = dock.menuItem === item ? null : item
                    onDragStarted: (item, at) => {
                        dock.menuItem = null;
                        item.lifted = true;
                        dock.dragAt = at;
                        dock.dragItem = item;
                    }
                    onDragMoved: at => dock.dragAt = at
                    onDragEnded: at => {
                        dock.dragAt = at;
                        dock.endDrag();
                    }

                    // A divider before the first app that is only here while it runs.
                    Rectangle {
                        visible: item.divider
                        anchors.left: parent.left
                        anchors.leftMargin: -0.5
                        anchors.verticalCenter: parent.verticalCenter
                        width: 1
                        height: dock.iconSize * 0.8
                        color: Theme.alpha(Theme.palette.m3Outline, 0.35)
                    }
                }
            }
        }
    }

    // Where a dragged app would land among the pins.
    Rectangle {
        readonly property Item before: {
            if (dock.dropIndex < 0)
                return null;
            let n = 0;
            for (let i = 0; i < apps.count; ++i) {
                const it = apps.itemAt(i);
                if (!it || !it.pinned || it === dock.dragItem)
                    continue;
                if (n++ === dock.dropIndex)
                    return it;
            }
            return null;
        }
        readonly property Item after: {
            let last = null;
            for (let i = 0; i < apps.count; ++i) {
                const it = apps.itemAt(i);
                if (it && it.pinned && it !== dock.dragItem)
                    last = it;
            }
            return last;
        }

        visible: dock.dropIndex >= 0
        x: before ? before.mapToItem(dock.contentItem, 0, 0).x - row.spacing / 2 - width / 2
                  : after ? after.mapToItem(dock.contentItem, after.width, 0).x + row.spacing / 2 - width / 2 : shelf.x + 12
        y: shelf.y + 10
        width: 3
        height: shelf.height - 20
        radius: 1.5
        color: Theme.palette.m3Primary
    }

    // The app under the pointer while it is dragged; a badge says it is
    // about to leave the Dock.
    IconImage {
        visible: dock.dragItem !== null
        x: dock.dragAt.x - width / 2
        y: dock.dragAt.y - height / 2
        implicitSize: dock.iconSize
        source: dock.dragItem ? (dock.dragItem.icon.startsWith("file:") ? dock.dragItem.icon : Shell.iconPath(dock.dragItem.icon, "application-x-executable")) : ""
        opacity: dock.dragRemoves ? 0.6 : 1

        Rectangle {
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: -4
            visible: dock.dragRemoves
            width: 20
            height: 20
            radius: 10
            color: "#ffb4ab"

            MaterialIcon {
                anchors.centerIn: parent
                text: "remove"
                font.pointSize: Theme.font.size.small
                color: "#690005"
            }
        }
    }

    DockMenu {
        id: menu

        item: dock.menuItem
        x: {
            if (!item)
                return 0;
            const p = item.mapToItem(dock.contentItem, item.width / 2, 0);
            return Math.max(8, Math.min(dock.width - width - 8, p.x - width / 2));
        }
        anchors.bottom: shelf.top
        anchors.bottomMargin: 10
        onClosed: dock.menuItem = null
    }

    // Clicking somewhere else focuses something else: the menu goes away.
    Connections {
        target: Atrium

        function onFocusedWindowChanged(): void {
            // A window taking focus means a click elsewhere; the panel taking the
            // keyboard itself leaves no window focused and must not close it.
            if (!Atrium.focusedWindow)
                return;
            dock.menuItem = null;
        }
    }
}
