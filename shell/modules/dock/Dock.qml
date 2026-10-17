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
    // The icon under the pointer: its name floats above it, as on a Mac.
    // The shelf itself never changes size for it.
    property Item hoverItem: null
    property string hoverName

    readonly property bool magnify: Atrium.settings["dock.magnify"] ?? false

    DockApps {
        id: dockApps

        entries: DesktopEntries
    }

    // Over the shelf, for magnification; -1 when away.
    readonly property real pointerX: shelfHover.hovered ? shelfHover.point.position.x : -1

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

    // A drag from another app over the shelf: the icon under it.
    property Item dropItem: null
    property point dropAt
    function itemAt(x: real): Item {
        for (let i = 0; i < apps.count; ++i) {
            const it = apps.itemAt(i);
            if (!it || it.leaving)
                continue;
            const box = it.mapToItem(dock.contentItem, 0, 0, it.width, it.height);
            if (x >= box.x && x < box.x + box.width)
                return it;
        }
        return null;
    }
    // Where among the pins dropped apps go.
    function pinIndexAt(x: real): int {
        let index = 0;
        for (let i = 0; i < apps.count; ++i) {
            const it = apps.itemAt(i);
            if (!it || !it.pinned)
                continue;
            const box = it.mapToItem(dock.contentItem, 0, 0, it.width, it.height);
            if (box.x + box.width / 2 < x)
                ++index;
        }
        return index;
    }
    onDropItemChanged: {
        for (let i = 0; i < apps.count; ++i) {
            const it = apps.itemAt(i);
            if (it)
                it.dropOver = it === dropItem;
        }
        spring.restart();
    }

    // Held over an app a moment, its window comes forward to drop into
    // (KDE Plasma's 250 ms; macOS springs folders the same way).
    Timer {
        id: spring

        interval: 250
        onTriggered: if (dock.dropItem) dockApps.springOpen(dock.dropItem.appId)
    }

    // Out of sight over a fullscreen app or a tiled space (or always, with
    // dock.autohide) until the pointer rests at the bottom of the screen, as
    // macOS's Dock: PanelReveal has the rule, the same as the menu bar's.
    readonly property bool fullscreen: output.fullscreen

    OutputState {
        id: output

        name: dock.screen?.name ?? ""
    }
    readonly property bool autohide: Atrium.settings["dock.autohide"] ?? false
    // Out until a window would go over it (KDE Plasma's Dodge Windows).
    readonly property bool dodge: !autohide && (Atrium.settings["dock.dodge"] ?? false)
    readonly property bool covered: dodge && output.dockCovered
    // Where the shelf rests, for the compositor to watch (Dodge Windows).
    readonly property rect shelfRest: Qt.rect(Math.round(((dock.screen?.width ?? 0) - shelf.width) / 2),
                                              (dock.screen?.height ?? 0) - shelfHeight - gap,
                                              Math.round(shelf.width), shelfHeight)
    readonly property var shelfWatch: [output.name, dodge && home && !empty, shelfRest]
    onShelfWatchChanged: {
        const on = dodge && home && !empty;
        Atrium.setDockShelf(output.name, shelfRest.x, shelfRest.y, on ? shelfRest.width : 0, shelfRest.height);
    }
    // Nothing pinned and nothing open: no Dock at all, not an empty shelf.
    readonly property bool empty: dockApps.count === 0
    // One Dock, on DockPlace.screen (dock.every_screen puts one on each).
    readonly property bool home: (Atrium.settings["dock.every_screen"] ?? false) || DockPlace.screen === output.name
    readonly property bool hides: fullscreen || output.tiled || autohide || covered || !home

    PanelReveal {
        id: reveal

        hides: dock.hides
        edge: dock.home && output.edge === "bottom"
        hovered: dock.home && (hover.hovered || drops.containsDrag)
        held: dock.menuItem !== null || dock.dragItem !== null
    }

    readonly property bool revealed: reveal.out && home && !empty

    // How deep it is, for the pointer keeping it out.
    readonly property string band: output.name + shelfHeight
    onBandChanged: Atrium.setPanelBand(output.name, "bottom", shelfHeight + gap)

    // Hidden over a fullscreen app it waits under the app, and comes over
    // when brought out (see Bar.qml).
    // Still sliding out counts as shown: it drops under the app once gone.
    readonly property bool shelfShown: shelf.anchors.bottomMargin > -(shelfHeight + 4)
    // Lifted over it only once brought over; going fullscreen it slides away
    // under the app, never flashing over it.
    property bool lifted: false
    onRevealedChanged: if (revealed && fullscreen) lifted = true
    readonly property bool over: fullscreen && lifted && (revealed || shelfShown)
    onOverChanged: if (!over) lifted = false
    WlrLayershell.layer: over ? WlrLayer.Overlay : WlrLayer.Top

    Connections {
        target: output

        // A tap of Super (Settings > Dock > Tap Super to show); with one
        // Dock, it comes to this screen.
        function onDockSummoned() {
            if (dock.empty)
                return;
            DockPlace.screen = output.name;
            reveal.summon();
        }
    }

    HoverHandler {
        id: hover
    }

    // Resting at the bottom of another screen moves the Dock there.
    Timer {
        interval: 300
        running: !dock.home && output.edge === "bottom"
        onTriggered: DockPlace.screen = output.name
    }

    // Where the shelf rests plus the gap under it down to the screen edge,
    // so the pointer that revealed the Dock is inside it at once (the shelf
    // itself is still sliding up from below then).
    Item {
        id: reach

        anchors.bottom: parent.bottom
        anchors.horizontalCenter: parent.horizontalCenter
        width: shelf.width
        height: shelf.height + dock.gap
    }

    anchors.bottom: true
    implicitWidth: Math.max(shelf.width + 40, menu.width + 40)
    // Room above the shelf for the hovered app's name, magnified icons and
    // its menu, however many rows that has.
    implicitHeight: shelfHeight + gap + Math.max(150, menu.height + menu.anchors.bottomMargin + 16)
    exclusiveZone: autohide || dodge || empty || output.tiled || !home ? 0 : shelfHeight + gap
    color: "transparent"
    WlrLayershell.namespace: "atrium-dock"
    // The keyboard while its menu is open, for Escape.
    WlrLayershell.keyboardFocus: menuItem ? WlrKeyboardFocus.Exclusive : WlrKeyboardFocus.None

    mask: Region {
        item: dock.revealed ? reach : null

        Region {
            item: dock.menuItem ? menu : null
        }
    }

    // The hovered app's name, above its icon (above it magnified too).
    Rectangle {
        id: hoverLabel

        z: 1

        // Where the icon is in the window; re-read as it magnifies and the
        // shelf slides.
        readonly property rect icon: {
            const it = dock.hoverItem;
            if (!it)
                return Qt.rect(0, 0, 0, 0);
            void (it.width + it.height + it.x + dock.pointerX + shelf.y + row.x);
            const box = it.mapToItem(dock.contentItem, 0, 0, it.width, it.height);
            // A magnified icon grows up out of its item.
            const grown = it.height * (it.magnification - 1);
            return Qt.rect(box.x, box.y - grown, box.width, box.height + grown);
        }

        width: labelText.implicitWidth + 20
        height: labelText.implicitHeight + 8
        radius: height / 2
        color: Theme.material.regular
        border.width: Theme.lens ? 0 : 1
        border.color: Theme.palette.separator
        opacity: dock.hoverItem && dock.revealed && !dock.dragItem && !dock.menuItem ? 1 : 0
        visible: opacity > 0

        Glass {}

        Behavior on opacity {
            Anim {
                duration: Theme.anim.small
            }
        }

        // Left where it was while it fades.
        Binding on x {
            when: dock.hoverItem !== null
            restoreMode: Binding.RestoreNone
            value: Math.max(4, Math.min(dock.width - hoverLabel.width - 4,
                                        hoverLabel.icon.x + hoverLabel.icon.width / 2 - hoverLabel.width / 2))
        }
        Binding on y {
            when: dock.hoverItem !== null
            restoreMode: Binding.RestoreNone
            value: hoverLabel.icon.y - hoverLabel.height - 10
        }

        StyledText {
            id: labelText

            anchors.centerIn: parent
            text: dock.hoverName
            font.pointSize: Theme.font.size.smaller
            font.weight: Font.Medium
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
        color: Theme.material.thin

        Glass {}

        Behavior on color {
            CAnim {}
        }
        border.width: Theme.lens ? 0 : 1
        border.color: Theme.palette.separator

        // No animation of its own: it follows the icons, which grow and
        // shrink smoothly as apps come and go.

        Behavior on anchors.bottomMargin {
            Anim {
                duration: Theme.anim.small
                easing.bezierCurve: Theme.anim.emphasizedDecel
            }
        }

        // A handler, not a MouseArea: it sees the pointer over the icons too
        // (their own MouseAreas take the hover from anything under them).
        HoverHandler {
            id: shelfHover

            onHoveredChanged: if (!hovered) dock.hoverItem = null
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

            // Where each icon is, for windows minimising into it: again once
            // the row settles after a change.
            onWidthChanged: iconReport.restart()
            onXChanged: iconReport.restart()

            Timer {
                id: iconReport

                interval: 150
                onTriggered: dockApps.reportIcons(row, dock.screen?.name ?? "")
            }

            Connections {
                target: dockApps

                function onDataChanged() { iconReport.restart(); }
                function onCountChanged() { iconReport.restart(); }
            }

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
                    onHoveredChanged: {
                        if (hovered) {
                            dock.hoverItem = item;
                            dock.hoverName = item.name;
                        } else if (dock.hoverItem === item && !shelfHover.hovered) {
                            dock.hoverItem = null;
                        }
                    }
                    magnification: {
                        if (!dock.magnify || dock.pointerX < 0)
                            return 1;
                        const center = x + width / 2 + row.x;
                        const d = Math.abs(dock.pointerX - center) / (dock.iconSize * 2.2);
                        return 1 + 0.55 * Math.max(0, Math.cos(Math.min(d, 1) * Math.PI / 2));
                    }
                    // Out from hiding for a moment so the hop is seen, but
                    // never over a fullscreen app.
                    onAttention: if (!dock.fullscreen && dock.home && !dock.empty) reveal.peek()
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
                        color: Theme.palette.separator
                    }
                }
            }
        }
    }

    // Files and apps dragged in from other apps.
    DropArea {
        id: drops

        x: reach.x
        y: reach.y
        width: reach.width
        height: reach.height
        keys: ["text/uri-list"]

        // Accepted as a copy, so the app dragging hands its data over on the
        // drop (a drag nobody accepted is called off before the drop).
        onEntered: drag => drag.accept(Qt.CopyAction)
        onPositionChanged: drag => {
            drag.accept(Qt.CopyAction);
            const at = mapToItem(dock.contentItem, drag.x, drag.y);
            dock.dropAt = at;
            // What is dragged is only read on the drop (reading it while
            // dragging asks the app for it early); apps are told apart then.
            dock.dropItem = dock.itemAt(at.x);
        }
        onExited: dock.dropItem = null
        onDropped: drop => {
            const target = dock.dropItem;
            // Qt reads files through the document portal; without one it
            // gets none, so KDE apps' own copy of the list stands in.
            let urls = drop.urls;
            if (urls.length === 0 && drop.formats.includes("application/x-kde4-urilist"))
                urls = dockApps.uriList(drop.getDataAsString("application/x-kde4-urilist"));
            dock.dropItem = null;
            if (dockApps.pinDropped(urls, dock.pinIndexAt(dock.dropAt.x))) {
                drop.accept(Qt.CopyAction);
            } else if (target) {
                dockApps.openWith(target.appId, urls);
                drop.accept(Qt.CopyAction);
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
        color: Theme.palette.accent
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
            color: Theme.palette.red

            MaterialIcon {
                anchors.centerIn: parent
                text: "remove"
                font.pointSize: Theme.font.size.small
                color: Theme.dark.label
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
        focus: item !== null
        Keys.onEscapePressed: dock.menuItem = null
    }

    // A press anywhere else closes the menu. (Not focus: the focused window's
    // record changes with its title, which some apps change all the time.)
    Connections {
        target: Atrium

        function onPointerPressed(layerNamespace: string): void {
            if (layerNamespace !== "atrium-dock")
                dock.menuItem = null;
        }
    }
}
