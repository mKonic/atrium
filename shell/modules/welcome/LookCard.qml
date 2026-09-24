import QtQuick
import shell.components
import shell.services
import Atrium

// Light or dark, drawn: a tiny desktop with a bar, a window and a Dock in
// that look, ringed in the accent when it's the one in use.
Column {
    id: root

    property string style: "dark"
    property bool chosen: false
    signal clicked

    readonly property bool light: style === "light"
    // That look's own colours, whichever is in use.
    readonly property var colors: Atrium.palette(light, Theme.accent)

    spacing: 8

    Rectangle {
        width: 176
        height: 112
        radius: 14
        color: "transparent"
        border.width: 3
        border.color: root.chosen ? Theme.palette.accent : "transparent"

        Rectangle {
            id: screen

            anchors.fill: parent
            anchors.margins: 5
            radius: 10
            clip: true
            color: Qt.tint(root.colors.windowBackground, root.colors.accentFill)

            Rectangle {
                width: parent.width
                height: 9
                color: root.colors.windowBackground
            }

            Rectangle {
                x: 22
                y: 20
                width: 104
                height: 60
                radius: 5
                color: root.colors.controlBackground
                border.width: 1
                border.color: root.colors.separator

                Row {
                    x: 6
                    y: 5
                    spacing: 3

                    Repeater {
                        model: [root.colors.red, root.colors.yellow, root.colors.green]

                        Rectangle {
                            required property color modelData

                            width: 5
                            height: 5
                            radius: 2.5
                            color: modelData
                        }
                    }
                }

                Rectangle {
                    x: 8
                    y: 18
                    width: 40
                    height: 6
                    radius: 3
                    color: Theme.palette.accent
                }
            }

            Rectangle {
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.bottom: parent.bottom
                anchors.bottomMargin: 4
                width: 64
                height: 12
                radius: 5
                color: root.colors.windowBackground
            }
        }

        MouseArea {
            anchors.fill: parent
            cursorShape: Qt.PointingHandCursor
            onClicked: root.clicked()
        }
    }

    StyledText {
        anchors.horizontalCenter: parent.horizontalCenter
        text: root.light ? "Light" : "Dark"
        font.weight: root.chosen ? Font.Medium : Font.Normal
    }
}
