pragma ComponentBehavior: Bound

import QtQuick
import shell.components
import shell.services

// A small popup menu: a title and rows of {icon, text, run}. The owner
// places it and closes it. A row may be disabled (`enabled: false`) or keep
// the menu open when picked (`keep: true`, a submenu opening in place).
Rectangle {
    id: root

    property string title: ""
    property var actions: []  // [{ icon, text, run, danger?, enabled?, keep?, trailing? }] or "-" for a separator
    // Rows line up: when any has an icon, all keep its room.
    readonly property bool iconColumn: actions.some(a => a !== "-" && (a.icon ?? "") !== "")
    signal picked

    // At least 220, wider for a long row.
    width: Math.max(220, column.widest + Theme.padding.small * 2)
    height: column.implicitHeight + Theme.padding.small * 2
    radius: Theme.rounding.normal
    color: Theme.material.regular

    Glass {}
    border.width: Theme.lens ? 0 : 1
    border.color: Theme.palette.separator

    Column {
        id: column

        // The widest row's own width (a Loader's implicitWidth is its row's).
        readonly property real widest: Math.max(0, ...children.map(c => c.implicitWidth ?? 0))

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
            color: Theme.palette.secondaryLabel
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
                color: Theme.palette.separator
            }
        }
    }

    Component {
        id: row

        Rectangle {
            id: item

            readonly property var action: (parent as Loader).modelData
            readonly property bool usable: action.enabled ?? true

            implicitWidth: content.implicitWidth + Theme.padding.normal * 2 + (trailingIcon.visible ? trailingIcon.implicitWidth + Theme.spacing.normal : 0)
            height: 34
            radius: Theme.rounding.small
            opacity: usable ? 1 : 0.4
            color: hover.containsMouse && usable ? Theme.palette.tertiaryFill : "transparent"

            Row {
                id: content

                anchors.verticalCenter: parent.verticalCenter
                anchors.left: parent.left
                anchors.leftMargin: Theme.padding.normal
                spacing: Theme.spacing.small

                MaterialIcon {
                    anchors.verticalCenter: parent.verticalCenter
                    visible: root.iconColumn
                    width: root.iconColumn ? Theme.font.size.normal * 1.6 : 0
                    horizontalAlignment: Text.AlignHCenter
                    text: item.action.icon ?? ""
                    font.pointSize: Theme.font.size.normal
                    color: item.action.danger ? Theme.palette.red : Theme.palette.label
                }

                StyledText {
                    anchors.verticalCenter: parent.verticalCenter
                    text: item.action.text
                    color: item.action.danger ? Theme.palette.red : Theme.palette.label
                }
            }

            // A submenu's chevron, at the end.
            MaterialIcon {
                id: trailingIcon

                anchors.verticalCenter: parent.verticalCenter
                anchors.right: parent.right
                anchors.rightMargin: Theme.padding.normal
                visible: text.length > 0
                text: item.action.trailing ?? ""
                font.pointSize: Theme.font.size.normal
                color: Theme.palette.secondaryLabel
            }

            MouseArea {
                id: hover

                anchors.fill: parent
                hoverEnabled: true
                enabled: item.usable
                onClicked: {
                    item.action.run();
                    if (!item.action.keep)
                        root.picked();
                }
            }
        }
    }
}
