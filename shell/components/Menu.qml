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
    property var actions: []  // [{ icon, text, run, danger?, enabled?, keep?, trailing?, hint? }] or "-" for a separator
    // Rows line up: when any has an icon, all keep its room.
    readonly property bool iconColumn: actions.some(a => a !== "-" && (a.icon ?? "") !== "")
    signal picked

    Accessible.role: Accessible.PopupMenu
    Accessible.name: title

    // At least 220, wider for a long row. The kit's menu: rounded 16, its
    // highlight 7px in from the edge.
    width: Math.max(220, column.widest + Theme.padding.smaller * 2)
    height: column.implicitHeight + Theme.padding.smaller * 2
    radius: 16
    color: Theme.material.regular

    Glass {}
    border.width: Theme.lens ? 0 : 1
    border.color: Theme.palette.separator

    Column {
        id: column

        // The widest row's own width (a Loader's implicitWidth is its row's).
        readonly property real widest: Math.max(0, ...children.map(c => c.implicitWidth ?? 0))

        anchors.fill: parent
        anchors.margins: Theme.padding.smaller

        StyledText {
            visible: root.title.length > 0
            width: parent.width
            leftPadding: Theme.padding.normal
            rightPadding: Theme.padding.normal
            height: 32
            text: root.title
            elide: Text.ElideMiddle
            font.weight: Font.Bold
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
            height: 15

            Rectangle {
                anchors.centerIn: parent
                width: parent.width - Theme.padding.normal * 2 - Theme.padding.small
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

            implicitWidth: content.implicitWidth + Theme.padding.normal * 2 + Theme.spacing.large + (trailingIcon.visible ? trailingIcon.implicitWidth + Theme.spacing.normal : 0)
                           + (hint.visible ? hint.implicitWidth + Theme.spacing.large * 2 : 0)
            height: 32
            radius: 11
            opacity: usable ? 1 : 0.4
            // Picked out in the accent, as a Mac's menus do.
            readonly property bool lit: hover.containsMouse && usable
            color: lit ? (item.action.danger ? Theme.palette.red : Theme.palette.accent) : "transparent"

            Accessible.role: Accessible.MenuItem
            Accessible.name: action.text ?? ""
            Accessible.description: action.hint ?? ""
            Accessible.onPressAction: if (usable) hover.clicked(null)

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
                    color: item.lit ? Theme.palette.labelOnAccent : item.action.danger ? Theme.palette.red : Theme.palette.label
                }

                StyledText {
                    anchors.verticalCenter: parent.verticalCenter
                    text: item.action.text
                    font.weight: Font.Medium
                    color: item.lit ? Theme.palette.labelOnAccent : item.action.danger ? Theme.palette.red : Theme.palette.label
                }
            }

            // Its shortcut ("⌃S"), at the end, as a Mac's menus show it.
            StyledText {
                id: hint

                anchors.verticalCenter: parent.verticalCenter
                anchors.right: trailingIcon.visible ? trailingIcon.left : parent.right
                anchors.rightMargin: trailingIcon.visible ? Theme.spacing.small : Theme.padding.normal
                visible: text.length > 0
                text: item.action.hint ?? ""
                font.weight: Font.Medium
                color: item.lit ? Theme.palette.labelOnAccent : Theme.palette.secondaryLabel
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
                color: item.lit ? Theme.palette.labelOnAccent : Theme.palette.secondaryLabel
            }

            MouseArea {
                id: hover

                anchors.fill: parent
                hoverEnabled: true
                enabled: item.usable
                onClicked: {
                    item.action.run?.();  // a row may only say something
                    if (!item.action.keep)
                        root.picked();
                }
            }
        }
    }
}
