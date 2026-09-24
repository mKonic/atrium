pragma ComponentBehavior: Bound

import QtQuick
import shell.components
import shell.services
import Atrium

// "Mod+Shift+E" drawn as keycaps, Mod shown as the key it stands for.
Row {
    id: root

    property string keys
    property bool recording: false

    readonly property string modName: {
        const m = Atrium.settings["shortcuts.modifier"] ?? "super";
        return m.charAt(0).toUpperCase() + m.slice(1);
    }

    spacing: 3

    Repeater {
        model: root.recording ? ["Press keys…"] : root.keys.split(/\+(?=.)/)

        Rectangle {
            id: cap

            required property string modelData
            readonly property string label: {
                const k = modelData === "Mod" ? root.modName : modelData;
                return ({ Return: "⏎", Left: "←", Right: "→", Up: "↑", Down: "↓", space: "Space", Tab: "⇥",
                          Escape: "Esc", BackSpace: "⌫", Delete: "Del", Page_Up: "PgUp", Page_Down: "PgDn",
                          comma: ",", period: ".", slash: "/", minus: "−", equal: "=", grave: "`",
                          XF86AudioRaiseVolume: "Volume +", XF86AudioLowerVolume: "Volume −",
                          XF86AudioMute: "Mute", XF86AudioMicMute: "Mic Mute", XF86AudioPlay: "Play",
                          XF86AudioPause: "Pause", XF86AudioNext: "Next", XF86AudioPrev: "Previous",
                          XF86MonBrightnessUp: "Brightness +", XF86MonBrightnessDown: "Brightness −" })[k] ?? k;
            }

            width: text.implicitWidth + 12
            height: 22
            radius: 5
            color: root.recording ? Theme.alpha(Theme.palette.m3Primary, 0.2) : Theme.alpha(Theme.palette.m3OnSurface, 0.1)
            border.width: 1
            border.color: root.recording ? Theme.palette.m3Primary : Theme.alpha(Theme.palette.m3OnSurface, 0.12)

            StyledText {
                id: text

                anchors.centerIn: parent
                text: cap.label
                font.pointSize: Theme.font.size.smaller
                font.weight: Font.Medium
            }
        }
    }
}
