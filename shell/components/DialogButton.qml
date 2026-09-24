import QtQuick
import shell.services

// A dialog's button, as in the polkit, session and access dialogs: the
// primary one filled with the accent.
Rectangle {
    id: button

    property string text
    property bool primary: false
    signal clicked

    width: 136
    height: 36
    radius: 18
    opacity: enabled ? 1 : 0.5
    color: primary ? (area.containsMouse ? Qt.lighter(Theme.palette.m3Primary, 1.08) : Theme.palette.m3Primary)
                   : (area.containsMouse ? Theme.alpha(Theme.palette.m3OnSurface, 0.16) : Theme.alpha(Theme.palette.m3OnSurface, 0.1))

    StyledText {
        anchors.centerIn: parent
        width: Math.min(implicitWidth, button.width - 20)
        elide: Text.ElideRight
        text: button.text
        font.weight: Font.DemiBold
        color: button.primary ? Theme.palette.m3OnPrimary : Theme.palette.m3OnSurface
    }

    MouseArea {
        id: area

        anchors.fill: parent
        hoverEnabled: true
        onClicked: button.clicked()
    }
}
