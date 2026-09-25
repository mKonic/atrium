pragma ComponentBehavior: Bound

import QtQuick
import Atrium.Shell
import shell.components
import shell.services
import Atrium

// The notification history, from the bell: grouped by app, newest first,
// with Do Not Disturb and Clear All.
PanelWindow {
    id: center

    readonly property bool dnd: Atrium.settings["notifications.dnd"] ?? false

    visible: Panels.open === "notifications"
    screen: Shell.screen(Atrium.focusedOutput?.name)
    anchors {
        top: true
        right: true
    }
    margins {
        top: 8
        right: 8
    }
    implicitWidth: 400
    implicitHeight: Math.min(panel.implicitHeight, (screen?.height ?? 900) * 0.75)
    exclusiveZone: 0
    color: "transparent"
    WlrLayershell.layer: WlrLayer.Top
    WlrLayershell.namespace: "atrium-notification-center"
    WlrLayershell.keyboardFocus: visible ? WlrKeyboardFocus.OnDemand : WlrKeyboardFocus.None

    onVisibleChanged: {
        if (visible)
            NotificationHistory.markRead();
    }

    Rectangle {
        id: panel

        anchors.fill: parent
        implicitHeight: body.implicitHeight + 16
        radius: 22
        color: Theme.material.regular

        Glass {}

        border.width: Theme.lens ? 0 : 1
        border.color: Theme.palette.separator
        focus: true
        Keys.onEscapePressed: Panels.open = ""

        Column {
            id: body

            x: 8
            y: 8
            width: parent.width - 16
            spacing: 8

            // Header: title, Do Not Disturb, Clear All.
            Item {
                width: parent.width
                height: 40

                StyledText {
                    anchors.left: parent.left
                    anchors.leftMargin: 8
                    anchors.verticalCenter: parent.verticalCenter
                    text: "Notifications"
                    font.pointSize: Theme.font.size.larger
                    font.weight: Font.DemiBold
                }

                Row {
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 6

                    Rectangle {
                        width: dndRow.implicitWidth + 20
                        height: 30
                        radius: 15
                        color: center.dnd ? Theme.palette.accent : Theme.palette.secondaryFill

                        Row {
                            id: dndRow

                            anchors.centerIn: parent
                            spacing: 4

                            MaterialIcon {
                                anchors.verticalCenter: parent.verticalCenter
                                text: center.dnd ? "do_not_disturb_on" : "do_not_disturb_off"
                                font.pointSize: Theme.font.size.normal
                                color: center.dnd ? Theme.palette.labelOnAccent : Theme.palette.label
                            }

                            StyledText {
                                anchors.verticalCenter: parent.verticalCenter
                                text: "Do Not Disturb"
                                font.pointSize: Theme.font.size.smaller
                                color: center.dnd ? Theme.palette.labelOnAccent : Theme.palette.label
                            }
                        }

                        TapHandler {
                            onTapped: Atrium.setSetting("notifications.dnd", !center.dnd)
                        }
                    }

                    Rectangle {
                        visible: NotificationHistory.items.length > 0
                        width: clearLabel.implicitWidth + 20
                        height: 30
                        radius: 15
                        color: clearHover.hovered ? Theme.palette.secondaryFill : "transparent"

                        StyledText {
                            id: clearLabel

                            anchors.centerIn: parent
                            text: "Clear All"
                            font.pointSize: Theme.font.size.smaller
                            color: Theme.palette.accent
                        }

                        HoverHandler {
                            id: clearHover
                        }

                        TapHandler {
                            onTapped: NotificationHistory.clear()
                        }
                    }
                }
            }

            // Nothing yet.
            Column {
                visible: NotificationHistory.items.length === 0
                width: parent.width
                topPadding: 28
                bottomPadding: 36
                spacing: 8

                MaterialIcon {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: "notifications_paused"
                    font.pointSize: 30
                    color: Theme.palette.tertiaryLabel
                }

                StyledText {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: "No notifications"
                    color: Theme.palette.secondaryLabel
                }
            }

            ListView {
                id: list

                visible: NotificationHistory.items.length > 0
                width: parent.width
                height: Math.min(contentHeight, (center.screen?.height ?? 900) * 0.75 - 72)
                clip: true
                spacing: 12
                boundsBehavior: Flickable.StopAtBounds
                model: NotificationHistory.groups

                delegate: Column {
                    id: group

                    required property var modelData

                    width: list.width
                    spacing: 6

                    Item {
                        width: parent.width
                        height: 26

                        Row {
                            anchors.left: parent.left
                            anchors.leftMargin: 8
                            anchors.verticalCenter: parent.verticalCenter
                            spacing: 8

                            IconImage {
                                anchors.verticalCenter: parent.verticalCenter
                                implicitSize: 18
                                source: group.modelData.icon
                            }

                            StyledText {
                                anchors.verticalCenter: parent.verticalCenter
                                text: group.modelData.app
                                font.weight: Font.Medium
                                font.pointSize: Theme.font.size.smaller
                                color: Theme.palette.secondaryLabel
                            }
                        }

                        MaterialIcon {
                            anchors.right: parent.right
                            anchors.rightMargin: 8
                            anchors.verticalCenter: parent.verticalCenter
                            text: "close"
                            font.pointSize: Theme.font.size.normal
                            color: Theme.palette.secondaryLabel

                            TapHandler {
                                onTapped: NotificationHistory.clearApp(group.modelData.app)
                            }
                        }
                    }

                    Repeater {
                        model: group.modelData.items

                        NotificationCard {
                            required property var modelData

                            width: group.width
                            compact: true
                            app: modelData.app
                            icon: modelData.icon
                            image: modelData.image ?? ""
                            summary: modelData.summary
                            body: modelData.body
                            time: modelData.time
                            onDismissed: NotificationHistory.remove(modelData.uid)
                        }
                    }
                }
            }
        }
    }
}
