import QtQuick
import shell.services

// The rounded container every bar item sits in.
Rectangle {
    color: Theme.material.pill
    radius: Theme.rounding.full
    implicitHeight: Theme.bar.inner

    Behavior on color {
        CAnim {}
    }
}
