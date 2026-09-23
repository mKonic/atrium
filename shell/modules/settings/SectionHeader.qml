import QtQuick
import qs.components
import qs.services

// A card's heading, with room for buttons on the right.
Item {
    id: root

    property string title
    property string subtitle
    default property alias actions: buttons.data

    implicitHeight: Math.max(column.implicitHeight, buttons.implicitHeight)

    Column {
        id: column

        anchors.left: parent.left
        anchors.right: buttons.left
        anchors.rightMargin: 12
        anchors.verticalCenter: parent.verticalCenter
        spacing: 2

        StyledText {
            text: root.title
            font.pointSize: Theme.font.size.larger
            font.weight: Font.DemiBold
        }

        StyledText {
            width: parent.width
            visible: text.length > 0
            text: root.subtitle
            wrapMode: Text.WordWrap
            font.pointSize: Theme.font.size.small
            color: Theme.palette.m3OnSurfaceVariant
        }
    }

    Row {
        id: buttons

        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        spacing: 8
    }
}
