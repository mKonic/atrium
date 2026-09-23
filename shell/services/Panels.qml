pragma Singleton

import Quickshell

// Which drop-down panel is open ("notifications", or "" for none). One at a
// time, like menus.
Singleton {
    property string open: ""

    function toggle(name: string): void {
        open = open === name ? "" : name;
    }
}
