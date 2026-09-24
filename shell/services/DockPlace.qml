pragma Singleton

import Quickshell

// Which screen has the Dock. As on macOS there is one, on the first screen
// until the pointer rests at the bottom of another (dock.every_screen puts
// one on each instead).
Singleton {
    property string screen: Quickshell.screens.length > 0 ? Quickshell.screens[0].name : ""
}
