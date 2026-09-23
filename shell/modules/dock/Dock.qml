pragma ComponentBehavior: Bound

import QtQuick
import Quickshell
import Quickshell.Wayland
import qs.components
import qs.services

// The Dock: pinned apps, then running ones that aren't pinned, on a frosted
// shelf along the bottom. The window is taller than the shelf so names and
// menus have room above it; only the shelf (and an open menu) take input.
PanelWindow {
    id: dock

    readonly property real iconSize: 48
    readonly property real shelfPadding: 8
    readonly property real shelfHeight: iconSize + shelfPadding * 2 + 6
    readonly property real gap: 8  // under the shelf
    readonly property bool magnify: Atrium.setting("dock.magnify", false)

    readonly property var pinned: (Atrium.setting("dock.pinned", []) ?? []).filter(id => DesktopEntries.byId(id) !== null)

    // Desktop entry id for a window, so windows and pins meet.
    function entryId(appId: string): string {
        return DesktopEntries.heuristicLookup(appId)?.id ?? appId;
    }

    readonly property var windowsByApp: {
        const map = {};
        for (const w of Atrium.windows) {
            const id = entryId(w.app_id);
            (map[id] = map[id] ?? []).push(w);
        }
        return map;
    }

    readonly property var apps: {
        const list = pinned.map(id => ({ id: id, pinned: true }));
        const seen = new Set(pinned);
        for (const w of Atrium.windows) {
            const id = entryId(w.app_id);
            if (!seen.has(id)) {
                seen.add(id);
                list.push({ id: id, pinned: false });
            }
        }
        return list;
    }

    property real pointerX: -1  // over the shelf, for magnification
    property var menuItem: null

    anchors.bottom: true
    implicitWidth: Math.max(shelf.width + 40, menu.width + 40)
    implicitHeight: shelfHeight + gap + 150
    exclusiveZone: shelfHeight + gap
    color: "transparent"
    WlrLayershell.namespace: "atrium-dock"

    mask: Region {
        item: shelf

        Region {
            item: dock.menuItem ? menu : null
        }
    }

    Rectangle {
        id: shelf

        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: dock.gap
        width: row.width + dock.shelfPadding * 2
        height: dock.shelfHeight
        radius: 22
        color: Theme.panel(Theme.palette.m3SurfaceContainer, 0.62)

        Behavior on color {
            CAnim {}
        }
        border.width: 1
        border.color: Theme.alpha(Theme.palette.m3Outline, 0.18)

        Behavior on width {
            Anim {
                duration: Theme.anim.small
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
            spacing: 8

            Repeater {
                model: ScriptModel {
                    values: dock.apps
                    objectProp: "id"
                }

                DockItem {
                    id: item

                    required property var modelData
                    required property int index

                    appId: modelData.id
                    pinned: dock.pinned.includes(modelData.id)
                    windows: dock.windowsByApp[modelData.id] ?? []
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

                    // A divider before the first app that is only here while it runs.
                    Rectangle {
                        visible: !item.pinned && item.index > 0 && dock.apps[item.index - 1]?.pinned === true
                        anchors.right: parent.left
                        anchors.rightMargin: 4 - 0.5
                        anchors.verticalCenter: parent.verticalCenter
                        width: 1
                        height: dock.iconSize * 0.8
                        color: Theme.alpha(Theme.palette.m3Outline, 0.35)
                    }
                }
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
            dock.menuItem = null;
        }
    }
}
