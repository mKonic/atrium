import QtQuick
import shell.services
import Atrium.Shell

// The rounded place a bar item sits in: bare on the wallpaper, or glass.
Rectangle {
    color: Theme.bar.pill

    Glass {
        material: GlassShape.Control
    }
    radius: Theme.rounding.full
    implicitHeight: Theme.bar.inner

    Behavior on color {
        CAnim {}
    }
}
