pragma ComponentBehavior: Bound

import QtQuick
import Atrium.Shell
import shell.components
import shell.modules.settings
import shell.services
import Atrium

// The shot to mark up: arrows, boxes, blurred patches and a crop, then Done
// saves over the file (and copies it again). Closing keeps it as it was.
FloatingWindow {
    id: root

    title: "Screenshot"
    color: Theme.palette.m3Surface
    visible: false
    implicitWidth: Math.max(640, Math.min(1200, canvas.imageSize.width + 48))
    implicitHeight: Math.max(460, Math.min(860, canvas.imageSize.height + 130))
    minimumSize: Qt.size(560, 400)
    onVisibleChanged: if (!visible) Capture.cancel()

    Item {
        anchors.fill: parent
        focus: true
        Keys.onEscapePressed: Capture.cancel()
        Keys.onPressed: event => {
            if (event.key === Qt.Key_Z && (event.modifiers & Qt.ControlModifier))
                canvas.undo();
        }

        Row {
            id: tools

            anchors.top: parent.top
            anchors.topMargin: 14
            anchors.left: parent.left
            anchors.leftMargin: 16
            spacing: 4

            Repeater {
                model: [
                    { tool: "arrow", icon: "arrow_outward", tip: "Arrow" },
                    { tool: "box", icon: "crop_square", tip: "Box" },
                    { tool: "blur", icon: "blur_on", tip: "Blur" },
                    { tool: "crop", icon: "crop", tip: "Crop" }
                ]

                ToolButton {
                    required property var modelData

                    icon: modelData.icon
                    chosen: canvas.tool === modelData.tool
                    onClicked: canvas.tool = modelData.tool
                }
            }

            Item {
                width: 10
                height: 1
            }

            Repeater {
                model: ["#ff3b30", "#ffcc00", "#34c759", "#0a84ff", "#ffffff", "#000000"]

                Rectangle {
                    id: swatch

                    required property string modelData

                    anchors.verticalCenter: parent.verticalCenter
                    width: 22
                    height: 22
                    radius: 11
                    color: modelData
                    border.width: canvas.color === Qt.color(modelData) ? 3 : 1
                    border.color: canvas.color === Qt.color(modelData) ? Theme.palette.m3Primary : Theme.alpha(Theme.palette.m3Outline, 0.4)

                    MouseArea {
                        anchors.fill: parent
                        anchors.margins: -3
                        onClicked: canvas.color = swatch.modelData
                    }
                }
            }

            Item {
                width: 10
                height: 1
            }

            ToolButton {
                icon: "undo"
                enabled: canvas.edited
                opacity: enabled ? 1 : 0.4
                onClicked: canvas.undo()
            }
        }

        Row {
            anchors.top: parent.top
            anchors.topMargin: 14
            anchors.right: parent.right
            anchors.rightMargin: 16
            spacing: 8

            PillButton {
                icon: "delete"
                text: "Delete"
                onClicked: Capture.deleteLast()
            }

            PillButton {
                primary: true
                text: "Done"
                onClicked: {
                    canvas.save();
                    Capture.cancel();
                }
            }
        }

        MarkupCanvas {
            id: canvas

            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: tools.bottom
            anchors.bottom: parent.bottom
            anchors.margins: 16
            source: root.visible ? Capture.lastFile : ""
        }
    }

    component ToolButton: Rectangle {
        id: button

        property string icon
        property bool chosen: false
        signal clicked

        width: 36
        height: 36
        radius: 10
        color: chosen ? Theme.alpha(Theme.palette.m3Primary, 0.25)
             : area.containsMouse ? Theme.alpha(Theme.palette.m3OnSurface, 0.1) : "transparent"

        MaterialIcon {
            anchors.centerIn: parent
            text: button.icon
            font.pointSize: Theme.font.size.large
            color: button.chosen ? Theme.palette.m3Primary : Theme.palette.m3OnSurface
        }

        MouseArea {
            id: area

            anchors.fill: parent
            hoverEnabled: true
            onClicked: button.clicked()
        }
    }
}
