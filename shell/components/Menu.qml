pragma ComponentBehavior: Bound

import QtQuick
import shell.components
import shell.services

// A small popup menu: a title and rows of {icon, text, run}. The owner
// places it and closes it.
Rectangle {
    id: root

    property string title: ""
    property var actions: []  // [{ icon, text, run, danger? }] or "-" for a separator
    signal picked

    width: 220
    height: column.implicitHeight + Theme.padding.small * 2
    radius: Theme.rounding.normal
    color: Theme.palette.m3SurfaceContainerHigh
    border.width: 1
    border.color: Theme.alpha(Theme.palette.m3OutlineVariant, 0.6)

    Column {
        id: column

        anchors.fill: parent
        anchors.margins: Theme.padding.small

        StyledText {
            visible: root.title.length > 0
            width: parent.width
            leftPadding: Theme.padding.normal
            rightPadding: Theme.padding.normal
            topPadding: Theme.padding.small
            bottomPadding: Theme.padding.small
            text: root.title
            elide: Text.ElideMiddle
            font.weight: Font.DemiBold
            font.pointSize: Theme.font.size.smaller
            color: Theme.palette.m3OnSurfaceVariant
        }

        Repeater {
            model: root.actions

            Loader {
                id: entry

                required property var modelData

                width: column.width
                sourceComponent: modelData === "-" ? separator : row
            }
        }
    }

    Component {
        id: separator

        Item {
            height: 9

            Rectangle {
                anchors.centerIn: parent
                width: parent.width - Theme.padding.normal * 2
                height: 1
                color: Theme.alpha(Theme.palette.m3OutlineVariant, 0.7)
            }
        }
    }

    Component {
        id: row

        Rectangle {
            id: item

            readonly property var action: (parent as Loader).modelData

            height: 34
            radius: Theme.rounding.small
            color: hover.containsMouse ? Theme.alpha(Theme.palette.m3OnSurface, 0.08) : "transparent"

            Row {
                anchors.verticalCenter: parent.verticalCenter
                anchors.left: parent.left
                anchors.leftMargin: Theme.padding.normal
                spacing: Theme.spacing.small

                MaterialIcon {
                    anchors.verticalCenter: parent.verticalCenter
                    text: item.action.icon ?? ""
                    font.pointSize: Theme.font.size.normal
                    color: item.action.danger ? "#ffb4ab" : Theme.palette.m3OnSurface
                }

                StyledText {
                    anchors.verticalCenter: parent.verticalCenter
                    text: item.action.text
                    color: item.action.danger ? "#ffb4ab" : Theme.palette.m3OnSurface
                }
            }

            MouseArea {
                id: hover

                anchors.fill: parent
                hoverEnabled: true
                onClicked: {
                    item.action.run();
                    root.picked();
                }
            }
        }
    }
}
