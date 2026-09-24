pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Shapes
import shell.components
import shell.services
import Atrium

// The accent colour, as macOS picks it: a row of swatches, the rainbow one
// (multicolour) first, the chosen one ringed and named underneath.
Column {
    id: root

    property string value
    property var choices: []
    signal picked(string value)

    spacing: 4

    Row {
        spacing: 6

        Repeater {
            model: root.choices

            Item {
                id: swatch

                required property string modelData
                readonly property bool current: modelData === root.value
                readonly property string color: Atrium.accentColor(modelData)

                width: 22
                height: 22

                // The ring around the chosen one.
                Rectangle {
                    anchors.fill: parent
                    radius: width / 2
                    color: "transparent"
                    border.width: 2
                    border.color: swatch.current ? Theme.palette.secondaryLabel : "transparent"
                }

                Rectangle {
                    anchors.centerIn: parent
                    width: 16
                    height: 16
                    radius: 8
                    visible: swatch.color !== ""
                    color: swatch.color || "transparent"
                    border.width: 1
                    border.color: Theme.palette.separator
                }

                Shape {
                    anchors.centerIn: parent
                    width: 16
                    height: 16
                    visible: swatch.color === ""
                    preferredRendererType: Shape.CurveRenderer

                    ShapePath {
                        strokeWidth: 0
                        strokeColor: "transparent"
                        fillGradient: ConicalGradient {
                            centerX: 8
                            centerY: 8
                            GradientStop { position: 0.0; color: Theme.palette.red }
                            GradientStop { position: 0.17; color: "#ff9f0a" }
                            GradientStop { position: 0.33; color: "#ffd60a" }
                            GradientStop { position: 0.5; color: "#32d74b" }
                            GradientStop { position: 0.67; color: "#0a84ff" }
                            GradientStop { position: 0.83; color: "#bf5af2" }
                            GradientStop { position: 1.0; color: Theme.palette.red }
                        }
                        PathAngleArc {
                            centerX: 8
                            centerY: 8
                            radiusX: 8
                            radiusY: 8
                            startAngle: 0
                            sweepAngle: 360
                        }
                    }
                }

                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: if (!swatch.current) root.picked(swatch.modelData)
                }
            }
        }
    }

    StyledText {
        anchors.right: parent.right
        text: Atrium.accentLabel(root.value)
        font.pointSize: Theme.font.size.smaller
        color: Theme.palette.secondaryLabel
    }
}
