pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Effects
import Atrium.Shell
import Atrium
import shell.components
import shell.services

// Alt+Tab, as a Mac's Command-Tab: the windows' app icons in a row on frosted
// glass in the middle of the screen, a soft square gliding to the one picked
// and its name underneath. The compositor runs it (which windows, which is
// picked, letting go); this draws it, and hovering or clicking an icon
// picks it.
PanelWindow {
    id: root

    readonly property var sw: Atrium.switcher
    readonly property bool shown: sw.shown === true
    readonly property var items: sw.items ?? []
    readonly property int index: sw.index ?? 0
    readonly property var current: items[index] ?? null
    // Big icons as on a Mac, smaller when there are many to fit the screen.
    // (a cell is the icon and a seventh of it either side)
    readonly property real icon: Math.max(40, Math.min(96, ((screen?.width ?? 1440) * 0.86 - 2 * pad) / Math.max(1, items.length) / 1.28))
    readonly property real cellPad: Math.round(icon * 0.14)
    readonly property real cell: icon + 2 * cellPad
    readonly property real pad: 14

    screen: Shell.screen(sw.output)
    visible: shown || card.opacity > 0
    implicitWidth: card.width + 100  // room for the shadow
    implicitHeight: card.height + 100
    exclusiveZone: 0
    color: "transparent"
    WlrLayershell.layer: WlrLayer.Overlay
    WlrLayershell.namespace: "atrium-switcher"
    WlrLayershell.keyboardFocus: WlrKeyboardFocus.None

    Rectangle {
        id: card

        anchors.centerIn: parent
        width: row.width + 2 * root.pad
        height: row.height + 2 * root.pad + labels.height + 6
        radius: 26
        color: Theme.material.regular
        border.width: Theme.lens ? 0 : 1
        border.color: Theme.palette.separator

        Glass {}

        // Comes up quickly, a touch larger as it settles; goes at once.
        opacity: root.shown ? 1 : 0
        scale: root.shown ? 1 : 0.94
        Behavior on opacity {
            Anim {
                duration: root.shown ? 140 : 90
            }
        }
        Behavior on scale {
            Anim {
                duration: 180
                easing.bezierCurve: Theme.anim.emphasizedDecel
            }
        }

        layer.enabled: true
        layer.effect: MultiEffect {
            shadowEnabled: !Theme.lens
            shadowColor: Theme.palette.shadow
            shadowBlur: 1
            shadowVerticalOffset: 10
        }

        // The picked one's square, gliding from icon to icon.
        Rectangle {
            x: root.pad + root.index * root.cell
            y: root.pad
            width: root.cell
            height: root.cell
            radius: Math.round(root.cell * 0.22)
            color: Theme.palette.tertiaryFill
            border.width: 1
            border.color: Theme.palette.separator

            Behavior on x {
                enabled: root.shown
                Anim {
                    duration: 160
                    easing.bezierCurve: Theme.anim.emphasizedDecel
                }
            }
        }

        Row {
            id: row

            x: root.pad
            y: root.pad

            Repeater {
                model: root.items

                Item {
                    id: cellItem

                    required property var modelData
                    required property int index
                    readonly property bool picked: index === root.index

                    width: root.cell
                    height: root.cell

                    IconImage {
                        anchors.centerIn: parent
                        width: root.icon
                        height: root.icon
                        source: cellItem.modelData.icon
                            ? (cellItem.modelData.icon.startsWith("/") ? "file://" + cellItem.modelData.icon
                                                                       : Shell.iconPath(cellItem.modelData.icon, "application-x-executable"))
                            : Icons.appIcon(cellItem.modelData.app_id)
                        // The picked one stands out a little.
                        scale: cellItem.picked ? 1.04 : 1
                        Behavior on scale {
                            Anim {
                                duration: 160
                            }
                        }
                    }

                    MouseArea {
                        anchors.fill: parent
                        hoverEnabled: true
                        onEntered: Atrium.switcherHover(cellItem.index)
                        onClicked: Atrium.switcherPick(cellItem.index)
                    }
                }
            }
        }

        // The app's name, and which of its windows (the title), under the
        // picked icon as on a Mac, kept inside the panel.
        Column {
            id: labels

            readonly property real under: root.pad + root.index * root.cell + root.cell / 2 - width / 2

            x: Math.max(root.pad, Math.min(card.width - root.pad - width, under))
            y: row.y + row.height + 6
            spacing: 1

            Behavior on x {
                enabled: root.shown
                Anim {
                    duration: 160
                    easing.bezierCurve: Theme.anim.emphasizedDecel
                }
            }

            StyledText {
                anchors.horizontalCenter: parent.horizontalCenter
                width: Math.min(implicitWidth, card.width - 2 * root.pad)
                horizontalAlignment: Text.AlignHCenter
                elide: Text.ElideRight
                text: root.current ? Icons.appName(root.current.app_id) : ""
                font.weight: Font.DemiBold
            }

            StyledText {
                anchors.horizontalCenter: parent.horizontalCenter
                width: Math.min(implicitWidth, card.width - 2 * root.pad)
                horizontalAlignment: Text.AlignHCenter
                elide: Text.ElideMiddle
                visible: text !== ""
                text: root.current && root.current.title.toLowerCase() !== Icons.appName(root.current.app_id).toLowerCase() ? root.current.title : ""
                font.pointSize: Theme.font.size.small
                color: Theme.palette.secondaryLabel
            }
        }
    }
}
