import QtQuick
import qs.services

// The rounded container every bar item sits in.
Rectangle {
    color: Theme.pill(Theme.palette.m3SurfaceContainer)
    radius: Theme.rounding.full
    implicitHeight: Theme.bar.inner

    GlassRim {}

    Behavior on color {
        CAnim {}
    }
}
