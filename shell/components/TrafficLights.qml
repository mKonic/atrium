import QtQuick
import shell.services

// A window's own minimize, full-screen and close buttons, for windows that
// draw their title bar themselves (as macOS 26's apps do, but on the right
// as atrium's title bars have them).
// Grey while the window is in the background; the glyphs show while the
// pointer is over any of them. `window` is the FloatingWindow.
Row {
    id: root

    required property var window
    signal closeRequested  // what closing means is the window's to say

    readonly property bool active: window?.active ?? true

    spacing: 9

    HoverHandler {
        id: hover
    }

    Repeater {
        // atrium's order, on the right of the window: minimize, full screen,
        // close last (as its title bars draw them).
        model: [
            { kind: "minimize", color: "#febc2e", glyph: "remove" },
            { kind: "zoom", color: "#28c840", glyph: "open_in_full" },
            { kind: "close", color: "#ff5f57", glyph: "close" }
        ]

        Rectangle {
            id: light

            required property var modelData

            width: 14
            height: 14
            radius: 7
            color: root.active || hover.hovered ? (press.pressed ? Qt.darker(modelData.color, 1.25) : modelData.color)
                                                : Theme.palette.fill
            border.width: 0.5
            border.color: Qt.rgba(0, 0, 0, 0.15)

            MaterialIcon {
                anchors.centerIn: parent
                visible: hover.hovered
                text: light.modelData.glyph
                font.pointSize: 7.5
                font.weight: Font.Black
                color: Qt.rgba(0, 0, 0, 0.55)
            }

            MouseArea {
                id: press

                anchors.fill: parent
                anchors.margins: -3
                onClicked: {
                    if (light.modelData.kind === "close")
                        root.closeRequested();
                    else if (light.modelData.kind === "minimize")
                        root.window.minimize();
                    else
                        root.window.toggleFullScreen();
                }
            }
        }
    }
}
