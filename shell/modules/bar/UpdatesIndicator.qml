import QtQuick
import Atrium
import shell.components
import shell.services

// Updates waiting, in the menu bar: a click opens Software Update. The
// system (PackageKit, or pacman), the AUR and atrium's own release together.
Pill {
    id: pill

    readonly property var system: Updates.available ? Updates : PacmanUpdates
    readonly property int count: system.count + AurUpdates.count + AtriumRelease.count
    readonly property bool installing: system.installing || AtriumRelease.installing
    readonly property int progress: system.installing ? system.progress : AtriumRelease.progress

    visible: count > 0 || installing
    implicitWidth: row.implicitWidth + Theme.padding.normal * 2

    Row {
        id: row

        anchors.centerIn: parent
        spacing: Theme.spacing.small / 2

        MaterialIcon {
            anchors.verticalCenter: parent.verticalCenter
            text: pill.installing ? "downloading" : "system_update_alt"
            font.pointSize: Theme.font.size.normal
        }

        StyledText {
            anchors.verticalCenter: parent.verticalCenter
            text: pill.installing ? `${pill.progress}%` : pill.count
            font.pointSize: Theme.font.size.smaller
        }
    }

    TapHandler {
        onTapped: Atrium.action("shell", "settings:Software Update")
    }
}
