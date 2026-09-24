import QtQuick
import shell.services

// A colour as #rrggbbaa, with a swatch of it (over a checkerboard, so the
// transparency shows).
Row {
    id: root

    property string value
    signal committed(string value)

    // "#rrggbbaa" → Qt's "#aarrggbb"
    readonly property color swatch: /^#[0-9a-fA-F]{8}$/.test(value) ? `#${value.slice(7, 9)}${value.slice(1, 7)}` : value

    spacing: 8

    Rectangle {
        width: 30
        height: 30
        radius: 8
        clip: true
        color: "#808080"

        Grid {
            columns: 4
            Repeater {
                model: 16
                Rectangle {
                    required property int index
                    width: 7.5
                    height: 7.5
                    color: (Math.floor(index / 4) + index) % 2 ? "#c8c8c8" : "#f0f0f0"
                }
            }
        }

        Rectangle {
            anchors.fill: parent
            color: root.swatch
            border.width: 1
            border.color: Theme.alpha(Theme.palette.m3Outline, 0.4)
            radius: 8
        }
    }

    TextControl {
        fieldWidth: 110
        value: root.value
        onCommitted: v => {
            if (/^#[0-9a-fA-F]{6}([0-9a-fA-F]{2})?$/.test(v))
                root.committed(v.length === 7 ? v + "ff" : v);
        }
    }
}
