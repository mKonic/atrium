import QtQuick
import shell.services

// A line of text, saved on Enter or when the field loses focus.
Rectangle {
    id: root

    property string value
    property int fieldWidth: 220
    property string placeholder: ""
    signal committed(string value)

    implicitWidth: fieldWidth
    implicitHeight: 30
    radius: 8
    color: Theme.palette.tertiaryFill
    border.width: 1
    border.color: input.activeFocus ? Theme.palette.focusRing : "transparent"

    onValueChanged: if (!input.activeFocus) input.text = value

    TextInput {
        id: input

        anchors.fill: parent
        anchors.leftMargin: 10
        anchors.rightMargin: 10
        verticalAlignment: TextInput.AlignVCenter
        text: root.value
        color: Theme.palette.label
        font.family: Theme.font.sans
        font.pointSize: Theme.font.size.small
        clip: true
        selectByMouse: true
        onAccepted: {
            if (text !== root.value)
                root.committed(text);
            focus = false;
        }
        onActiveFocusChanged: if (!activeFocus && text !== root.value) root.committed(text)
        Keys.onEscapePressed: {
            text = root.value;
            focus = false;
        }

        Text {
            anchors.verticalCenter: parent.verticalCenter
            visible: !input.text && root.placeholder
            text: root.placeholder
            font: input.font
            color: Theme.palette.tertiaryLabel
        }
    }
}
