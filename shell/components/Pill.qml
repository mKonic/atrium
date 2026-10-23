import QtQuick
import shell.services

// The rounded place a bar item sits in: bare on the wallpaper, or glass.
Rectangle {
    color: Theme.bar.pill

    Glass {}
    radius: Theme.rounding.full
    implicitHeight: Theme.bar.inner

    Behavior on color {
        CAnim {}
    }
}
