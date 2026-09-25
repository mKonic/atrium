pragma Singleton

import QtQuick
import Atrium.Shell
import Atrium

// Which drop-down panel is open ("notifications", or "" for none). One at a
// time, like menus.
Singleton {
    id: root

    property string open: ""
    property string deep: ""  // the open panel shows one of its pages
    signal back(string name)

    // Clicking a panel's button again steps back out of a page first, as
    // macOS Control Center does.
    // The layer surface each panel is drawn on.
    readonly property var namespaces: ({
        "control": "atrium-control-center",
        "notifications": "atrium-notification-center",
        "system": "atrium-system-menu"
    })

    // A press anywhere but the open panel closes it, as on macOS. Presses on
    // the bar are its buttons' own: they toggle the panel themselves.
    property Connections outsideClicks: Connections {
        target: Atrium

        function onPointerPressed(layerNamespace: string): void {
            const mine = root.namespaces[root.open];
            if (mine && layerNamespace !== mine && layerNamespace !== "atrium-bar")
                root.open = "";
        }
    }

    function toggle(name: string): void {
        if (open === name && deep === name) {
            back(name);
            return;
        }
        open = open === name ? "" : name;
    }
}
