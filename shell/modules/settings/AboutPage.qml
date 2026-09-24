import QtQuick
import shell.components
import shell.services
import Atrium

// About: the machine, as the logo menu's card shows it, on a card here.
Rectangle {
    id: root

    width: parent?.width ?? 0
    height: facts.implicitHeight + 56
    radius: 14
    color: Theme.palette.m3SurfaceContainer
    border.width: 1
    border.color: Theme.alpha(Theme.palette.m3Outline, 0.12)

    SystemFacts {
        id: facts

        anchors.horizontalCenter: parent.horizontalCenter
        y: 28
        width: Math.min(parent.width - 48, 440)
        uptime: root.visible ? SystemInfo.uptime() : ""
    }
}
