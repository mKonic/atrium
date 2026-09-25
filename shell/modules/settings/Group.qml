import QtQuick
import shell.components
import shell.services

// A titled card of rows.
Column {
    id: root

    property string title
    property string subtitle
    default property alias content: body.data
    property alias headerActions: header.actions

    width: parent?.width ?? 0
    spacing: 8

    // A card with nothing to say above it has no heading.
    SectionHeader {
        id: header

        visible: implicitHeight > 0
        width: parent.width
        title: root.title
        subtitle: root.subtitle
    }

    Rectangle {
        width: parent.width
        height: body.implicitHeight + 16
        radius: 14
        color: Theme.palette.groupedBackground
        border.width: 1
        border.color: Theme.palette.separator

        Column {
            id: body

            x: 8
            y: 8
            width: parent.width - 16
        }
    }
}
