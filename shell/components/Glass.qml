import QtQuick
import Atrium.Shell
import shell.services

// Goes inside a Rectangle that is Liquid Glass: atrium draws the glass from
// its exact shape (atrium-glass-v1), so its edge is smooth at any size.
GlassShape {
    anchors.fill: parent
    radius: parent?.radius ?? 0
    visible: Theme.lens
}
