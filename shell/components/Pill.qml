import QtQuick
import qs.services

// The rounded container every bar item sits in.
Rectangle {
    color: Theme.palette.m3SurfaceContainer
    radius: Theme.rounding.full
    implicitHeight: Theme.bar.inner

    Behavior on color {
        CAnim {}
    }
}
