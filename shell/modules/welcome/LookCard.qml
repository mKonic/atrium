import QtQuick
import shell.components
import shell.services

// Light or dark, drawn: a tiny desktop with a bar, a window and a Dock in
// that look, ringed in the accent when it's the one in use.
Column {
    id: root

    property string style: "dark"
    property bool chosen: false
    signal clicked

    readonly property bool light: style === "light"

    spacing: 8

    Rectangle {
        width: 176
        height: 112
        radius: 14
        color: "transparent"
        border.width: 3
        border.color: root.chosen ? Theme.palette.m3Primary : "transparent"

        Rectangle {
            id: screen

            anchors.fill: parent
            anchors.margins: 5
            radius: 10
            clip: true
            color: root.light ? "#d9dde6" : "#1d2029"

            Rectangle {
                width: parent.width
                height: 9
                color: root.light ? "#f4f5f8" : "#2b2e38"
            }

            Rectangle {
                x: 22
                y: 20
                width: 104
                height: 60
                radius: 5
                color: root.light ? "#ffffff" : "#343844"
                border.width: 1
                border.color: root.light ? Qt.rgba(0, 0, 0, 0.08) : Qt.rgba(1, 1, 1, 0.08)

                Row {
                    x: 6
                    y: 5
                    spacing: 3

                    Repeater {
                        model: ["#ff5f57", "#febc2e", "#28c840"]

                        Rectangle {
                            required property string modelData

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
                    color: Theme.palette.m3Primary
                }
            }

            Rectangle {
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.bottom: parent.bottom
                anchors.bottomMargin: 4
                width: 64
                height: 12
                radius: 5
                color: root.light ? Qt.rgba(1, 1, 1, 0.8) : Qt.rgba(0.2, 0.21, 0.26, 0.9)
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
