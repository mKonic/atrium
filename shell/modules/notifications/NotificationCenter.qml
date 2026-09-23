pragma ComponentBehavior: Bound

import QtQuick
import Quickshell
import Quickshell.Widgets
import Quickshell.Wayland
import qs.components
import qs.services
import Atrium

// The notification history, from the bell: grouped by app, newest first,
// with Do Not Disturb and Clear All.
PanelWindow {
    id: center

    readonly property bool dnd: Atrium.settings["notifications.dnd"] ?? false

    visible: Panels.open === "notifications"
    screen: Quickshell.screens.find(s => s.name === Atrium.focusedOutput?.name) ?? Quickshell.screens[0]
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

    // Clicking a window closes it, like a menu.
    Connections {
        target: Atrium

        function onFocusedWindowChanged(): void {
            // A window taking focus means a click elsewhere; the panel taking the
            // keyboard itself leaves no window focused and must not close it.
            if (!Atrium.focusedWindow)
                return;
            if (Panels.open === "notifications")
                Panels.open = "";
        }
    }

    Rectangle {
        id: panel

        anchors.fill: parent
        implicitHeight: body.implicitHeight + 16
        radius: 22
        color: Theme.panel(Theme.palette.m3Surface, 0.8)
        border.width: 1
        border.color: Theme.alpha(Theme.palette.m3Outline, 0.2)
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
                        color: center.dnd ? Theme.palette.m3Primary : Theme.palette.m3SurfaceContainerHigh

                        Row {
                            id: dndRow

                            anchors.centerIn: parent
                            spacing: 4

                            MaterialIcon {
                                anchors.verticalCenter: parent.verticalCenter
                                text: center.dnd ? "do_not_disturb_on" : "do_not_disturb_off"
                                font.pointSize: Theme.font.size.normal
                                color: center.dnd ? Theme.palette.m3OnPrimary : Theme.palette.m3OnSurface
                            }

                            StyledText {
                                anchors.verticalCenter: parent.verticalCenter
                                text: "Do Not Disturb"
                                font.pointSize: Theme.font.size.smaller
                                color: center.dnd ? Theme.palette.m3OnPrimary : Theme.palette.m3OnSurface
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
                        color: clearHover.hovered ? Theme.palette.m3SurfaceContainerHigh : "transparent"

                        StyledText {
                            id: clearLabel

                            anchors.centerIn: parent
                            text: "Clear All"
                            font.pointSize: Theme.font.size.smaller
                            color: Theme.palette.m3Primary
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
                    color: Theme.alpha(Theme.palette.m3OnSurfaceVariant, 0.6)
                }

                StyledText {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: "No notifications"
                    color: Theme.palette.m3OnSurfaceVariant
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
                                color: Theme.palette.m3OnSurfaceVariant
                            }
                        }

                        MaterialIcon {
                            anchors.right: parent.right
                            anchors.rightMargin: 8
                            anchors.verticalCenter: parent.verticalCenter
                            text: "close"
                            font.pointSize: Theme.font.size.normal
                            color: Theme.palette.m3OnSurfaceVariant

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
