import QtQuick
import shell.components
import shell.services
import Atrium

// Notifications: a dot while there are unread ones, struck through during
// Do Not Disturb. Opens the notification center.
Pill {
    id: root

    // For screen readers (AT-SPI).
    Accessible.role: Accessible.Button
    Accessible.name: qsTr("Notifications")
    Accessible.description: dnd ? qsTr("Do Not Disturb") : NotificationHistory.unread > 0 ? qsTr("%1 unread").arg(NotificationHistory.unread) : ""
    Accessible.onPressAction: Panels.toggle("notifications")

    readonly property bool dnd: Atrium.settings["notifications.dnd"] ?? false
    readonly property bool open: Panels.open === "notifications"

    implicitWidth: implicitHeight
    color: open ? Theme.bar.selected : Theme.bar.pill

    Glass {}

    MaterialIcon {
        anchors.centerIn: parent
        text: root.dnd ? "notifications_off" : NotificationHistory.unread > 0 ? "notifications_unread" : "notifications"
        fill: root.open ? 1 : 0
        font.pointSize: Theme.font.size.larger
        color: NotificationHistory.unread > 0 && !root.dnd ? Theme.bar.palette.accent : Theme.bar.palette.label
    }

    TapHandler {
        onTapped: Panels.toggle("notifications")
    }
}
