pragma ComponentBehavior: Bound

import QtQuick
import shell.components
import shell.services

// One of a few: side-by-side segments, as macOS shows short choices.
Rectangle {
    id: root

    property string value
    property var choices: []
    signal picked(string value)

    // Words title-cased; scale factors as percentages.
    function label(c: string): string {
        if (/^[0-9.]+$/.test(c))
            return `${Math.round(Number(c) * 100)}%`;
        return c.split(/[-_]/).map(w => w.charAt(0).toUpperCase() + w.slice(1)).join(" ");
    }

    implicitWidth: row.implicitWidth + 6
    implicitHeight: 30
    radius: 9
    color: Theme.palette.tertiaryFill

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
                color: current ? Theme.palette.accent : area.containsMouse ? Theme.palette.tertiaryFill : "transparent"

                StyledText {
                    id: text

                    anchors.centerIn: parent
                    text: root.label(segment.modelData)
                    font.pointSize: Theme.font.size.small
                    font.weight: segment.current ? Font.DemiBold : Font.Normal
                    color: segment.current ? Theme.palette.labelOnAccent : Theme.palette.label
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
