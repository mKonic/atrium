import QtQuick
import shell.components
import shell.services

// One piece of macOS 26's Control Center: its own pane of Liquid Glass,
// floating over the desktop, with the large continuous corners of a capsule,
// a circle or a rounded square.
Rectangle {
    color: Theme.material.regular
    border.width: Theme.lens ? 0 : 1
    border.color: Theme.palette.separator

    Glass {}
}
