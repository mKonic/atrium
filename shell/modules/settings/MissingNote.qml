import QtQuick
import Atrium
import shell.components
import shell.services

// Said in place of a page's controls when what it fronts isn't there:
// "NetworkManager isn't running." and what that means.
Rectangle {
    id: root

    property string needs
    property string explanation
    // What's said: the missing service by default.
    property string message: Requirements.missing[needs] ?? ""

    visible: message.length > 0
    width: parent?.width ?? 0
    height: visible ? body.implicitHeight + 32 : 0
    radius: 14
    color: Theme.palette.m3SurfaceContainer
    border.width: 1
    border.color: Theme.alpha(Theme.palette.m3Outline, 0.12)

    MaterialIcon {
        id: icon

        x: 16
        y: 16
        text: "info"
        font.pointSize: Theme.font.size.large
        color: Theme.palette.m3OnSurfaceVariant
    }

    Column {
        id: body

        anchors.left: icon.right
        anchors.leftMargin: 12
        anchors.right: parent.right
        anchors.rightMargin: 16
        y: 16
        spacing: 2

        StyledText {
            width: parent.width
            text: root.message
            wrapMode: Text.WordWrap
            font.weight: Font.Medium
        }

        StyledText {
            width: parent.width
            visible: text.length > 0
            text: root.explanation
            wrapMode: Text.WordWrap
            font.pointSize: Theme.font.size.small
            color: Theme.palette.m3OnSurfaceVariant
        }
    }
}
