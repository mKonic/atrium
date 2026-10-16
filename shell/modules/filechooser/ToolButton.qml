import QtQuick
import shell.components
import shell.services

// A square icon button in the dialog's header (GTK's image buttons).
Rectangle {
    id: root

    property string icon
    property string name
    property bool checked: false
    signal clicked

    width: 32
    height: 32
    radius: 8
    color: checked ? Theme.palette.secondaryFill : area.containsMouse ? Theme.palette.tertiaryFill : "transparent"
    Accessible.role: Accessible.Button
    Accessible.name: name
    Accessible.checkable: true
    Accessible.checked: checked

    MaterialIcon {
        anchors.centerIn: parent
        text: root.icon
        color: Theme.palette.label
    }

    MouseArea {
        id: area

        anchors.fill: parent
        hoverEnabled: true
        onClicked: root.clicked()
    }
}
