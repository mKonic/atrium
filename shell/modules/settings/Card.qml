pragma ComponentBehavior: Bound

import QtQuick
import shell.services

// A group of settings on one rounded card, rows split by hairlines.
Rectangle {
    id: root

    property var rows: []
    property bool showPage: false  // search results say where each lives

    visible: rows.length > 0
    width: parent?.width ?? 0
    height: column.implicitHeight
    radius: 14
    color: Theme.palette.groupedBackground
    border.width: 1
    border.color: Theme.palette.separator

    Column {
        id: column

        width: parent.width

        Repeater {
            model: root.rows

            Column {
                id: slot

                required property var modelData
                required property int index

                width: column.width

                Rectangle {
                    visible: slot.index > 0
                    x: 16
                    width: parent.width - 32
                    height: 1
                    color: Theme.palette.separator
                }

                SettingRow {
                    width: parent.width
                    setting: slot.modelData
                    showPage: root.showPage
                }
            }
        }
    }
}
