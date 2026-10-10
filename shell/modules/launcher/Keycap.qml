import QtQuick
import shell.components
import shell.services

// A key or chord as the palette writes it: "Ctrl+K" as two caps.
Row {
    id: root

    property string keys
    property bool dim: false

    spacing: 3

    Repeater {
        model: root.keys.length > 0 ? root.keys.split("+") : []

        Rectangle {
            required property string modelData

            width: Math.max(20, cap.implicitWidth + 10)
            height: 20
            radius: 5
            color: Theme.palette.tertiaryFill

            StyledText {
                id: cap

                anchors.centerIn: parent
                text: ({ "Enter": "↵", "Up": "↑", "Down": "↓", "Left": "←", "Right": "→", "Mod": qsTr("Super"),
                         "Shift": "⇧", "Tab": "⇥", "Escape": "Esc", "Backspace": "⌫" })[parent.modelData] ?? parent.modelData
                font.pointSize: Theme.font.size.small
                color: root.dim ? Theme.palette.secondaryLabel : Theme.palette.label
            }
        }
    }
}
