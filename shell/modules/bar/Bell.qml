import QtQuick
import shell.components
import shell.services
import Atrium

// Notifications: a dot while there are unread ones, struck through during
// Do Not Disturb. Opens the notification center.
Pill {
    id: root

    readonly property bool dnd: Atrium.settings["notifications.dnd"] ?? false
    readonly property bool open: Panels.open === "notifications"

    implicitWidth: implicitHeight
    color: open ? Theme.palette.accentFill : Theme.material.pill

    Glass {}

    MaterialIcon {
        anchors.centerIn: parent
        text: root.dnd ? "notifications_off" : NotificationHistory.unread > 0 ? "notifications_unread" : "notifications"
        fill: root.open ? 1 : 0
        font.pointSize: Theme.font.size.normal
        color: NotificationHistory.unread > 0 && !root.dnd ? Theme.palette.accent : Theme.palette.label
    }

    TapHandler {
        onTapped: Panels.toggle("notifications")
    }
}
