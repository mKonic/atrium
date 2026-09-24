import QtQuick
import shell.components
import shell.services

// A row of a page's own: its name (and a note) on the left, its control
// on the right, as the settings rows are laid out.
Item {
    id: row

    property string title
    property string note
    default property alias control: slot.data

    width: parent?.width ?? 0
    implicitHeight: Math.max(56, labels.implicitHeight + 24, slot.height + 24)

    Column {
        id: labels

        // In a Group's card, which already pads its rows by 8.
        x: 8
        anchors.verticalCenter: parent.verticalCenter
        width: parent.width - 16 - slot.width - 16
        spacing: 2

        StyledText {
            width: parent.width
            text: row.title
            wrapMode: Text.WordWrap
            font.weight: Font.Medium
        }

        StyledText {
            width: parent.width
            visible: row.note.length > 0
            text: row.note
            wrapMode: Text.WordWrap
            font.pointSize: Theme.font.size.small
            color: Theme.palette.m3OnSurfaceVariant
        }
    }

    Item {
        id: slot

        anchors.right: parent.right
        anchors.rightMargin: 8
        anchors.verticalCenter: parent.verticalCenter
        width: childrenRect.width
        height: childrenRect.height
    }
}
