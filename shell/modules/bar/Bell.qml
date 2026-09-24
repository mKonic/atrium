import QtQuick
import qs.components
import qs.services
import Atrium

// Notifications: a dot while there are unread ones, struck through during
// Do Not Disturb. Opens the notification center.
Pill {
    id: root

    readonly property bool dnd: Atrium.settings["notifications.dnd"] ?? false
    readonly property bool open: Panels.open === "notifications"

    implicitWidth: implicitHeight
    color: open ? Theme.palette.m3SecondaryContainer : Theme.pill(Theme.palette.m3SurfaceContainer)

    MaterialIcon {
        anchors.centerIn: parent
        text: root.dnd ? "notifications_off" : NotificationHistory.unread > 0 ? "notifications_unread" : "notifications"
        fill: root.open ? 1 : 0
        font.pointSize: Theme.font.size.normal
        color: NotificationHistory.unread > 0 && !root.dnd ? Theme.palette.m3Primary : Theme.palette.m3OnSurface
    }

    TapHandler {
        onTapped: Panels.toggle("notifications")
    }
}
