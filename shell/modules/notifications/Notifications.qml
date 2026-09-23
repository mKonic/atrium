pragma ComponentBehavior: Bound

import QtQuick
import Quickshell
import Quickshell.Services.Notifications
import Quickshell.Wayland
import qs.components
import qs.services
import Atrium

// atrium's notification server: every notification goes into the history,
// and pops up at the top right unless Do Not Disturb is on (critical ones
// always do).
Scope {
    id: root

    property list<QtObject> popups: []
    readonly property bool dnd: Atrium.settings["notifications.dnd"] ?? false

    // An image that is really a theme icon ("notify-send -i name").
    function isIconUrl(url: string): bool {
        return (url ?? "").startsWith("image://icon/");
    }

    // Its icon if the theme has it (or the icon it sent as its image), else
    // its app's icon; a generic one rather than a wrong guess.
    function iconFor(n: var): string {
        const icon = n.appIcon;
        if (icon && (icon.startsWith("/") || icon.startsWith("file:")))
            return icon.startsWith("/") ? `file://${icon}` : icon;
        const named = icon ? Quickshell.iconPath(icon, true) : "";
        if (named)
            return named;
        if (isIconUrl(n.image)) {
            // A theme icon, or (Discord's lives in pixmaps) its app's.
            const name = n.image.slice("image://icon/".length).split("?")[0];
            const themed = Quickshell.iconPath(name, true);
            if (themed)
                return themed;
            // No fallback here: with one, the lookup skips pixmaps.
            const app = DesktopEntries.byId(name) ?? DesktopEntries.heuristicLookup(name);
            if (app)
                return Quickshell.iconPath(app.icon);
        }
        const entry = DesktopEntries.byId(n.desktopEntry) ?? DesktopEntries.byId((n.appName ?? "").toLowerCase());
        if (entry)
            return Quickshell.iconPath(entry.icon);
        return Quickshell.iconPath("preferences-desktop-notification", "dialog-information");
    }

    // A real picture (a photo, an album cover), not an icon.
    function pictureOf(n: var): string {
        return n.image && !isIconUrl(n.image) ? n.image : "";
    }

    NotificationServer {
        keepOnReload: false
        bodySupported: true
        bodyMarkupSupported: true
        actionsSupported: true
        imageSupported: true
        persistenceSupported: true

        onNotification: n => {
            n.tracked = true;
            NotificationHistory.add({
                app: n.appName || Icons.appName(n.desktopEntry) || "Notification",
                icon: root.iconFor(n),
                summary: n.summary,
                body: n.body,
                image: root.pictureOf(n),
                urgency: n.urgency,
                desktopEntry: n.desktopEntry
            });
            if (!root.dnd || n.urgency === NotificationUrgency.Critical)
                root.popups = [n, ...root.popups].slice(0, 5);
        }
    }

    function drop(n: var): void {
        root.popups = root.popups.filter(p => p !== n);
    }

    PanelWindow {
        id: stack

        screen: Quickshell.screens.find(s => s.name === Atrium.focusedOutput?.name) ?? Quickshell.screens[0]
        anchors {
            top: true
            right: true
        }
        margins {
            top: 8
            right: 8
        }
        implicitWidth: 380
        implicitHeight: Math.max(1, column.implicitHeight)
        exclusiveZone: 0
        color: "transparent"
        // The notification center, when open, already shows them all.
        visible: root.popups.length > 0 && Panels.open !== "notifications"
        WlrLayershell.layer: WlrLayer.Overlay
        WlrLayershell.namespace: "atrium-notifications"

        mask: Region {
            item: column
        }

        Column {
            id: column

            width: parent.width
            spacing: 8

            Repeater {
                model: root.popups

                NotificationCard {
                    id: card

                    required property var modelData
                    readonly property var n: modelData

                    width: column.width
                    app: n.appName || Icons.appName(n.desktopEntry)
                    icon: root.iconFor(n)
                    image: root.pictureOf(n)
                    summary: n.summary
                    body: n.body
                    time: Date.now()
                    critical: n.urgency === NotificationUrgency.Critical
                    actions: n.actions.map(a => ({ text: a.text, invoke: () => a.invoke() }))

                    // Slides in from the right edge.
                    transform: Translate {
                        id: slide

                        x: 0
                    }
                    Component.onCompleted: enter.start()

                    ParallelAnimation {
                        id: enter

                        Anim {
                            target: slide
                            property: "x"
                            from: 400
                            to: 0
                            duration: Theme.anim.normal
                            easing.bezierCurve: Theme.anim.emphasizedDecel
                        }
                        Anim {
                            target: card
                            property: "opacity"
                            from: 0
                            to: 1
                            duration: Theme.anim.small
                        }
                    }

                    // Goes on its own after a while, unless it matters or the
                    // pointer is on it.
                    Timer {
                        running: !card.critical && !card.hovered && !card.expanded
                        interval: card.n.expireTimeout > 0 ? card.n.expireTimeout : 5000
                        onTriggered: root.drop(card.n)
                    }

                    Connections {
                        target: card.n

                        function onClosed(): void {
                            root.drop(card.n);
                        }
                    }

                    onDismissed: {
                        root.drop(n);
                        n.dismiss();
                    }
                    onClicked: {
                        const main = n.actions.find(a => a.identifier === "default");
                        if (main)
                            main.invoke();
                        root.drop(n);
                    }
                }
            }
        }
    }
}
