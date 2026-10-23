import QtQuick
import shell.services

// A dialog's button, as in the polkit, session and access dialogs: the
// primary one filled with the accent.
Rectangle {
    id: button

    property string text
    property bool primary: false
    signal clicked

    // For screen readers (AT-SPI): a button that says what it does.
    Accessible.role: Accessible.Button
    Accessible.name: text
    Accessible.onPressAction: clicked()

    // The kit's alert button: a capsule 28pt high (37 here).
    width: 136
    height: 37
    radius: height / 2
    opacity: enabled ? 1 : 0.5
    color: primary ? (area.containsMouse ? Qt.lighter(Theme.palette.accent, 1.08) : Theme.palette.accent)
                   : (area.containsMouse ? Theme.palette.fill : Theme.palette.control)

    StyledText {
        anchors.centerIn: parent
        width: Math.min(implicitWidth, button.width - 20)
        elide: Text.ElideRight
        text: button.text
        font.weight: Font.Medium
        color: button.primary ? Theme.palette.labelOnAccent : Theme.palette.label
    }

    MouseArea {
        id: area

        anchors.fill: parent
        hoverEnabled: true
        onClicked: button.clicked()
    }
}
