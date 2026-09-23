pragma ComponentBehavior: Bound

import QtQuick
import qs.components
import qs.services

// One of a few: side-by-side segments, as macOS shows short choices.
Rectangle {
    id: root

    property string value
    property var choices: []
    signal picked(string value)

    function label(c: string): string {
        return c.split(/[-_]/).map(w => w.charAt(0).toUpperCase() + w.slice(1)).join(" ");
    }

    implicitWidth: row.implicitWidth + 6
    implicitHeight: 30
    radius: 9
    color: Theme.alpha(Theme.palette.m3OnSurface, 0.08)

    Row {
        id: row

        anchors.centerIn: parent
        spacing: 2

        Repeater {
            model: root.choices

            Rectangle {
                id: segment

                required property string modelData
                readonly property bool current: modelData === root.value

                width: text.implicitWidth + 22
                height: 24
                radius: 7
                color: current ? Theme.palette.m3Primary : area.containsMouse ? Theme.alpha(Theme.palette.m3OnSurface, 0.08) : "transparent"

                StyledText {
                    id: text

                    anchors.centerIn: parent
                    text: root.label(segment.modelData)
                    font.pointSize: Theme.font.size.small
                    font.weight: segment.current ? Font.DemiBold : Font.Normal
                    color: segment.current ? Theme.palette.m3OnPrimary : Theme.palette.m3OnSurface
                }

                MouseArea {
                    id: area

                    anchors.fill: parent
                    hoverEnabled: true
                    onClicked: if (!segment.current) root.picked(segment.modelData)
                }
            }
        }
    }
}
