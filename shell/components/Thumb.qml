import QtQuick
import shell.services

// The knob of a slider or switch, as macOS 26 draws it: a solid light
// capsule at rest; held, it grows a little and turns to clear glass, a lens
// over the track (`backdrop`, a sibling it doesn't sit inside) that magnifies
// the fill under it and bends it at the rim.
Item {
    id: root

    property bool pressed: false
    property Item backdrop: null
    property real glass: pressed && backdrop ? 1 : 0
    readonly property real pad: 6

    Behavior on glass {
        Anim {
            duration: Theme.anim.small
            easing.bezierCurve: Theme.anim.standard
        }
    }

    scale: 1 + 0.2 * glass

    Rectangle {
        anchors.fill: parent
        radius: height / 2
        color: Theme.palette.thumb
        border.width: Theme.light ? 0.5 : 0
        border.color: Theme.palette.fill
        opacity: 1 - root.glass
        visible: opacity > 0.01
    }

    // The track around the knob, live, for the lens to look through.
    ShaderEffectSource {
        id: behind

        readonly property point at: root.backdrop && root.parent ? root.parent.mapToItem(root.backdrop, root.x, root.y) : Qt.point(0, 0)

        sourceItem: root.glass > 0.01 ? root.backdrop : null
        sourceRect: Qt.rect(at.x - root.pad, at.y - root.pad, root.width + 2 * root.pad, root.height + 2 * root.pad)
        live: true
        visible: false
    }

    ShaderEffect {
        anchors.fill: parent
        visible: root.glass > 0.01
        opacity: root.glass

        property var source: behind
        property size knob: Qt.size(width, height)
        property real pad: root.pad
        property real magnify: 1.25
        property color tint: Theme.light ? Qt.rgba(1, 1, 1, 0.3) : Qt.rgba(1, 1, 1, 0.1)
        property color rim: Theme.light ? Qt.rgba(1, 1, 1, 0.95) : Qt.rgba(1, 1, 1, 0.8)

        fragmentShader: "shaders/lens.frag.qsb"
    }
}
