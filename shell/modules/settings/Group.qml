import QtQuick
import qs.components
import qs.services

// A titled card of rows.
Column {
    id: root

    property string title
    property string subtitle
    default property alias content: body.data
    property alias headerActions: header.actions

    width: parent?.width ?? 0
    spacing: 8

    SectionHeader {
        id: header

        width: parent.width
        title: root.title
        subtitle: root.subtitle
    }

    Rectangle {
        width: parent.width
        height: body.implicitHeight + 16
        radius: 14
        color: Theme.palette.m3SurfaceContainer
        border.width: 1
        border.color: Theme.alpha(Theme.palette.m3Outline, 0.12)

        Column {
            id: body

            x: 8
            y: 8
            width: parent.width - 16
        }
    }
}
