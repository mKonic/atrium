pragma ComponentBehavior: Bound

import QtQuick
import qs.services

// A group of settings on one rounded card, rows split by hairlines.
Rectangle {
    id: root

    property var rows: []
    property bool showPage: false  // search results say where each lives

    visible: rows.length > 0
    width: parent?.width ?? 0
    height: column.implicitHeight
    radius: 14
    color: Theme.palette.m3SurfaceContainer
    border.width: 1
    border.color: Theme.alpha(Theme.palette.m3Outline, 0.12)

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
                    color: Theme.alpha(Theme.palette.m3Outline, 0.14)
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
