pragma ComponentBehavior: Bound

import QtQuick
import shell.components
import shell.services

// A list of short texts: chips to remove, a field to add.
Flow {
    id: root

    property var value: []
    signal committed(var value)

    spacing: 6

    Repeater {
        model: root.value

        Rectangle {
            id: chip

            required property string modelData
            required property int index

            width: row.implicitWidth + 16
            height: 28
            radius: 14
            color: Theme.alpha(Theme.palette.m3OnSurface, 0.08)

            Row {
                id: row

                anchors.centerIn: parent
                spacing: 4

                StyledText {
                    anchors.verticalCenter: parent.verticalCenter
                    text: chip.modelData
                    font.pointSize: Theme.font.size.small
                }

                MaterialIcon {
                    anchors.verticalCenter: parent.verticalCenter
                    text: "close"
                    font.pointSize: Theme.font.size.small
                    color: Theme.palette.m3OnSurfaceVariant

                    MouseArea {
                        anchors.fill: parent
                        anchors.margins: -4
                        onClicked: root.committed(root.value.filter((_, i) => i !== chip.index))
                    }
                }
            }
        }
    }

    TextControl {
        fieldWidth: 160
        placeholder: "Add…"
        value: ""
        onCommitted: v => {
            if (v.trim())
                root.committed(root.value.concat([v.trim()]));
        }
    }
}
