import QtQuick
import shell.services

// Liquid Glass's light on a panel (appearance.liquid_glass): a bright rim
// where the edge catches the light, strongest along the top, and a soft
// sheen over the upper part. Goes first inside the panel, under its content.
Item {
    id: root

    readonly property real radius: parent?.radius ?? 0

    anchors.fill: parent
    visible: Theme.liquid

    // The sheen: light falling in from above.
    Rectangle {
        anchors.fill: parent
        radius: root.radius
        gradient: Gradient {
            GradientStop { position: 0.0; color: Qt.rgba(1, 1, 1, Theme.light ? 0.30 : 0.14) }
            GradientStop { position: 0.45; color: Qt.rgba(1, 1, 1, 0) }
        }
    }

    // The rim: a hairline of light, and a darker one just inside for depth.
    Rectangle {
        anchors.fill: parent
        radius: root.radius
        color: "transparent"
        border.width: 1
        border.color: Qt.rgba(1, 1, 1, Theme.light ? 0.65 : 0.32)
    }

    Rectangle {
        anchors.fill: parent
        anchors.margins: 1
        radius: Math.max(0, root.radius - 1)
        color: "transparent"
        border.width: 1
        border.color: Qt.rgba(0, 0, 0, Theme.light ? 0.05 : 0.18)
    }
}
