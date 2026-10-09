import QtQuick
import QtQuick.Effects
import shell.components
import shell.services

// A round picture, or initials on a tint.
Rectangle {
    id: avatar

    property var user: ({})
    property real size: 40

    width: size
    height: size
    radius: size / 2
    color: Theme.dark.tertiaryFill
    clip: true

    StyledText {
        anchors.centerIn: parent
        visible: !(avatar.user.icon ?? "")
        text: avatar.user.initials ?? ""
        font.pointSize: avatar.size * 0.3
        font.weight: Font.DemiBold
        color: Theme.dark.label
    }

    Image {
        anchors.fill: parent
        visible: !!(avatar.user.icon ?? "")
        source: avatar.user.icon ?? ""
        fillMode: Image.PreserveAspectCrop
        sourceSize: Qt.size(avatar.size * 2, avatar.size * 2)
        // clip only cuts to the square: the circle masks it.
        layer.enabled: true
        layer.effect: MultiEffect {
            maskEnabled: true
            maskSource: circle
            maskThresholdMin: 0.5
            maskSpreadAtMin: 1
        }
    }

    Rectangle {
        id: circle

        anchors.fill: parent
        radius: width / 2
        visible: false
        layer.enabled: true
    }
}
