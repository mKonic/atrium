import QtQuick
import QtQuick.Shapes
import shell.services

// An atrium window's own background, less a hole where a pane of Liquid Glass
// floats (the window is see-through there, for atrium to draw the glass
// behind). Without Liquid Glass, plain background everywhere.
Shape {
    id: root

    required property Item pane  // the glass, a Rectangle with its radius

    anchors.fill: parent
    preferredRendererType: Shape.CurveRenderer

    ShapePath {
        fillColor: Theme.palette.windowBackground
        fillRule: ShapePath.OddEvenFill
        strokeWidth: -1

        PathRectangle {
            width: root.width
            height: root.height
        }
        PathRectangle {
            x: Theme.lens ? root.pane.x : 0
            y: Theme.lens ? root.pane.y : 0
            width: Theme.lens ? root.pane.width : 0
            height: Theme.lens ? root.pane.height : 0
            radius: root.pane.radius
        }
    }
}
