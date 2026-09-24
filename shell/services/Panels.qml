pragma Singleton

import Atrium.Shell

// Which drop-down panel is open ("notifications", or "" for none). One at a
// time, like menus.
Singleton {
    property string open: ""
    property string deep: ""  // the open panel shows one of its pages
    signal back(string name)

    // Clicking a panel's button again steps back out of a page first, as
    // macOS Control Center does.
    function toggle(name: string): void {
        if (open === name && deep === name) {
            back(name);
            return;
        }
        open = open === name ? "" : name;
    }
}
