pragma ComponentBehavior: Bound

import QtQuick
import shell.components
import shell.services
import Atrium

// GTK's Other Locations, "On This Computer": the computer itself and its
// own disks, with how much room is left; one not mounted mounts on a click.
Rectangle {
    id: root

    required property ChooserPlaces places
    signal open(string path)

    radius: 10
    color: Theme.palette.groupedBackground
    border.width: 1
    border.color: Theme.palette.separator

    StyledText {
        id: heading

        x: 14
        y: 10
        text: qsTr("On This Computer")
        font.pointSize: Theme.font.size.smaller
        font.weight: Font.DemiBold
        color: Theme.palette.secondaryLabel
    }

    ListView {
        anchors.top: heading.bottom
        anchors.topMargin: 6
        anchors.bottom: parent.bottom
        x: 4
        width: parent.width - 8
        clip: true
        model: root.places.otherLocations

        delegate: Rectangle {
            id: disk

            required property var modelData

            width: ListView.view.width
            height: 36
            radius: 6
            color: area.containsMouse ? Theme.palette.tertiaryFill : "transparent"

            Row {
                x: 10
                anchors.verticalCenter: parent.verticalCenter
                spacing: 10

                MaterialIcon {
                    anchors.verticalCenter: parent.verticalCenter
                    text: disk.modelData.icon
                    color: Theme.palette.accent
                }

                StyledText {
                    anchors.verticalCenter: parent.verticalCenter
                    text: disk.modelData.name
                    font.pointSize: Theme.font.size.small
                }
            }

            StyledText {
                anchors.right: parent.right
                anchors.rightMargin: 12
                anchors.verticalCenter: parent.verticalCenter
                text: disk.modelData.mounted
                      ? (disk.modelData.free ? qsTr("%1 / %2 available").arg(disk.modelData.free).arg(disk.modelData.size) : disk.modelData.path)
                      : qsTr("Not mounted")
                font.pointSize: Theme.font.size.smaller
                color: Theme.palette.secondaryLabel
            }

            MouseArea {
                id: area

                anchors.fill: parent
                hoverEnabled: true
                onClicked: disk.modelData.mounted ? root.open(disk.modelData.path) : root.places.mount(disk.modelData.device)
            }
        }
    }
}
