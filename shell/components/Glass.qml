import QtQuick
import Atrium.Shell
import shell.services

// Goes inside a Rectangle that is Liquid Glass: atrium draws the glass from
// its exact shape (atrium-glass-v1), of its `material` (regular unless set:
// GlassShape.Thin, Thick, Control or Clear, as the Rectangle's Theme.material).
GlassShape {
    anchors.fill: parent
    radius: parent?.radius ?? 0
    visible: Theme.lens
}
