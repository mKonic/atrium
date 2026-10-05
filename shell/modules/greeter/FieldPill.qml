import QtQuick
import shell.services

// A frosted pill for a text field.
Rectangle {
    anchors.horizontalCenter: parent?.horizontalCenter
    width: 240
    height: 36
    radius: 18
    color: Theme.dark.tertiaryFill
    border.width: 1
    border.color: Theme.dark.separator
}
