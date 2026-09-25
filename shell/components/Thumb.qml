import QtQuick
import QtQuick.Effects
import shell.services

// The knob of a slider or switch, as macOS draws it: a raised disc with a
// fine edge and a soft shadow, so it stands out even on a white card.
Rectangle {
    radius: width / 2
    color: Theme.palette.thumb
    border.width: 1
    border.color: Theme.palette.fill

    layer.enabled: true
    layer.effect: MultiEffect {
        shadowEnabled: true
        shadowColor: Theme.palette.shadow
        shadowBlur: 0.25
        shadowVerticalOffset: 1
        shadowOpacity: 0.6
    }
}
