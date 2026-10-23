pragma ComponentBehavior: Bound

import QtQuick
import shell.components
import shell.services

// One of a few: side-by-side segments, as macOS shows short choices.
Rectangle {
    id: root

    property string value
    property var choices: []
    // What it chooses, for screen readers (its label is the row's).
    property string name
    signal picked(string value)

    Accessible.role: Accessible.Grouping
    Accessible.name: name

    // Words title-cased; scale factors as percentages.
    function label(c: string): string {
        if (/^[0-9.]+$/.test(c))
            return `${Math.round(Number(c) * 100)}%`;
        return c.split(/[-_]/).map(w => w.charAt(0).toUpperCase() + w.slice(1)).join(" ");
    }

    // The kit's segmented control: segments flush in a faint track, the
    // chosen one filled with the accent.
    implicitWidth: row.implicitWidth
    implicitHeight: 32
    radius: 11
    color: Theme.palette.secondaryFill

    Row {
        id: row

        anchors.centerIn: parent
        spacing: 0

        Repeater {
            model: root.choices

            Rectangle {
                id: segment

                required property string modelData
                readonly property bool current: modelData === root.value

                width: text.implicitWidth + 26
                height: 32
                radius: 11
                color: current ? Theme.palette.accent : area.pressed ? Theme.palette.fill : area.containsMouse ? Theme.palette.quaternaryFill : "transparent"

                Accessible.role: Accessible.RadioButton
                Accessible.name: qsTranslate("settings", root.label(modelData))
                Accessible.checkable: true
                Accessible.checked: current
                Accessible.onPressAction: if (!current) root.picked(modelData)

                StyledText {
                    id: text

                    anchors.centerIn: parent
                    text: qsTranslate("settings", root.label(segment.modelData))
                    font.weight: segment.current ? Font.Medium : Font.Normal
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
