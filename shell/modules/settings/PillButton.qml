import QtQuick
import shell.components
import shell.services

// A small rounded button: text, optionally an icon; `primary` fills it.
Rectangle {
    id: root

    property string text
    property string icon
    property bool primary: false
    signal clicked

    implicitWidth: row.implicitWidth + 24
    implicitHeight: 30
    radius: 15
    opacity: enabled ? 1 : 0.5
    color: primary ? (area.containsMouse ? Qt.lighter(Theme.palette.m3Primary, 1.06) : Theme.palette.m3Primary)
                   : (area.containsMouse ? Theme.alpha(Theme.palette.m3OnSurface, 0.14) : Theme.alpha(Theme.palette.m3OnSurface, 0.08))

    Row {
        id: row

        anchors.centerIn: parent
        spacing: 4

        MaterialIcon {
            anchors.verticalCenter: parent.verticalCenter
            visible: root.icon.length > 0
            text: root.icon
            font.pointSize: Theme.font.size.normal
            color: root.primary ? Theme.palette.m3OnPrimary : Theme.palette.m3OnSurface
        }

        StyledText {
            anchors.verticalCenter: parent.verticalCenter
            text: root.text
            font.pointSize: Theme.font.size.small
            font.weight: Font.Medium
            color: root.primary ? Theme.palette.m3OnPrimary : Theme.palette.m3OnSurface
        }
    }

    MouseArea {
        id: area

        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onClicked: root.clicked()
    }
}
