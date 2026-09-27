import QtQuick
import shell.services

// A text field inside a form: the text is read when the form is sent.
Rectangle {
    id: root

    property alias text: input.text
    property string placeholder: ""
    property bool password: false
    signal accepted

    function focusField(): void {
        input.forceActiveFocus();
    }

    implicitWidth: 260
    implicitHeight: 32
    radius: 8
    color: Theme.palette.tertiaryFill
    border.width: 1
    border.color: input.activeFocus ? Theme.palette.focusRing : "transparent"

    TextInput {
        id: input

        anchors.fill: parent
        anchors.leftMargin: 10
        anchors.rightMargin: 10
        verticalAlignment: TextInput.AlignVCenter
        echoMode: root.password ? TextInput.Password : TextInput.Normal
        color: Theme.palette.label
        font.family: Theme.font.sans
        font.pointSize: Theme.font.size.small
        clip: true
        selectByMouse: true
        activeFocusOnTab: true  // Tab steps through a form's fields (a sheet's Popup doesn't on its own)
        onAccepted: root.accepted()
        Keys.onTabPressed: nextItemInFocusChain(true)?.forceActiveFocus(Qt.TabFocusReason)
        Keys.onBacktabPressed: nextItemInFocusChain(false)?.forceActiveFocus(Qt.BacktabFocusReason)

        Text {
            anchors.verticalCenter: parent.verticalCenter
            visible: !input.text && root.placeholder
            text: root.placeholder
            font: input.font
            color: Theme.palette.tertiaryLabel
        }
    }
}
