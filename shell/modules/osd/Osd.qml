pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Effects
import Atrium.Shell
import Atrium
import shell.components
import shell.services

// The media keys, and what they changed: volume, microphone and brightness
// show for a moment under the bar's right end, as macOS shows them. Volume
// changed from anywhere else shows too.
PanelWindow {
    id: osd

    readonly property AudioNode sink: Audio.sink
    readonly property AudioNode source: Audio.source
    readonly property real volume: sink?.volume ?? 0
    readonly property bool muted: sink?.muted ?? false
    readonly property var player: Mpris.active

    property string kind: "volume"  // volume, brightness, mic
    property bool shown: false
    property bool ready: false  // the first values arriving are not changes

    function show(what: string): void {
        if (!ready || Panels.open === "control")  // its own sliders already say it
            return;
        kind = what;
        shown = true;
        hideTimer.restart();
    }

    function setVolume(direction: int, fine: bool): void {
        const a = sink;
        if (!a)
            return;
        a.muted = false;
        a.volume = Levels.step(a.volume, direction, fine);
        show("volume");  // at the ends nothing changes, but the key still answers
    }

    function setBrightness(direction: int, fine: bool): void {
        if (!Brightness.available)
            return;
        Brightness.set(Math.round(Levels.step(Brightness.value / 100, direction, fine) * 100));
        show("brightness");
    }

    Connections {
        target: Atrium

        function onShellAction(name: string): void {
            switch (name) {
            case "volume-up": osd.setVolume(1, false); break;
            case "volume-up-fine": osd.setVolume(1, true); break;
            case "volume-down": osd.setVolume(-1, false); break;
            case "volume-down-fine": osd.setVolume(-1, true); break;
            case "volume-mute":
                if (osd.sink) {
                    osd.sink.muted = !osd.sink.muted;
                    osd.show("volume");
                }
                break;
            case "mic-mute":
                if (osd.source) {
                    osd.source.muted = !osd.source.muted;
                    osd.show("mic");
                }
                break;
            case "brightness-up": osd.setBrightness(1, false); break;
            case "brightness-up-fine": osd.setBrightness(1, true); break;
            case "brightness-down": osd.setBrightness(-1, false); break;
            case "brightness-down-fine": osd.setBrightness(-1, true); break;
            case "media-play-pause": if (osd.player?.canTogglePlaying) osd.player.togglePlaying(); break;
            case "media-next": if (osd.player?.canGoNext) osd.player.next(); break;
            case "media-previous": if (osd.player?.canGoPrevious) osd.player.previous(); break;
            }
        }
    }

    onVolumeChanged: show("volume")
    onMutedChanged: show("volume")


    Timer {
        running: true
        interval: 2000
        onTriggered: osd.ready = true
    }

    Timer {
        id: hideTimer

        interval: 1500
        onTriggered: osd.shown = false
    }

    readonly property real level: kind === "brightness" ? Brightness.value / 100 : kind === "mic" ? (source?.volume ?? 0) : volume
    readonly property bool off: kind === "mic" ? (source?.muted ?? false) : kind === "volume" && muted
    readonly property string glyph: kind === "brightness" ? (level > 0.5 ? "brightness_high" : "brightness_low")
        : kind === "mic" ? (off ? "mic_off" : "mic")
        : off || level === 0 ? "volume_off" : level < 0.34 ? "volume_mute" : level < 0.67 ? "volume_down" : "volume_up"
    readonly property string title: kind === "brightness" ? "Display" : kind === "mic" ? (off ? "Microphone Off" : "Microphone")
        : (sink?.label || "Sound")

    screen: Shell.screen(Atrium.focusedOutput?.name)
    visible: shown || card.opacity > 0
    anchors {
        top: true
        right: true
    }
    margins {
        top: 8
        right: 8
    }
    implicitWidth: 320
    implicitHeight: 76
    exclusiveZone: 0
    color: "transparent"
    mask: Region {}  // never in the way of the pointer
    WlrLayershell.layer: WlrLayer.Overlay
    WlrLayershell.namespace: "atrium-osd"

    Rectangle {
        id: card

        anchors.fill: parent
        radius: 22
        color: Theme.panel(Theme.palette.m3Surface, 0.82)

        GlassRim {}

        border.width: 1
        border.color: Theme.alpha(Theme.palette.m3Outline, 0.2)
        opacity: osd.shown ? 1 : 0
        scale: osd.shown ? 1 : 0.94
        transformOrigin: Item.TopRight

        Behavior on opacity {
            Anim {
                duration: Theme.anim.small
            }
        }

        Behavior on scale {
            Anim {
                duration: Theme.anim.small
                easing.bezierCurve: Theme.anim.emphasizedDecel
            }
        }

        Rectangle {
            id: badge

            anchors.left: parent.left
            anchors.leftMargin: 14
            anchors.verticalCenter: parent.verticalCenter
            width: 40
            height: 40
            radius: 20
            color: osd.off ? Theme.alpha(Theme.palette.m3OnSurface, 0.12) : Theme.palette.m3Primary

            MaterialIcon {
                anchors.centerIn: parent
                text: osd.glyph
                fill: 1
                color: osd.off ? Theme.palette.m3OnSurface : Theme.palette.m3OnPrimary
            }
        }

        StyledText {
            id: label

            anchors.left: badge.right
            anchors.leftMargin: 12
            anchors.right: percent.left
            anchors.rightMargin: 8
            anchors.top: badge.top
            anchors.topMargin: -1
            text: osd.title
            elide: Text.ElideRight
            font.weight: Font.DemiBold
        }

        StyledText {
            id: percent

            anchors.right: parent.right
            anchors.rightMargin: 16
            anchors.baseline: label.baseline
            text: osd.off ? "" : `${Math.round(osd.level * 100)}%`
            font.pointSize: Theme.font.size.small
            color: Theme.palette.m3OnSurfaceVariant
        }

        // The level, with the 16 steps the keys move by marked on it.
        Rectangle {
            id: track

            anchors.left: label.left
            anchors.right: parent.right
            anchors.rightMargin: 16
            anchors.bottom: badge.bottom
            anchors.bottomMargin: 2
            height: 6
            radius: 3
            color: Theme.alpha(Theme.palette.m3OnSurface, 0.14)

            Rectangle {
                width: Math.max(track.height, track.width * (osd.off ? 0 : osd.level))
                height: parent.height
                radius: parent.radius
                visible: !osd.off && osd.level > 0
                color: Theme.palette.m3Primary

                Behavior on width {
                    Anim {
                        duration: 120
                    }
                }
            }

            Repeater {
                model: 15

                Rectangle {
                    required property int index

                    x: track.width * (index + 1) / 16 - 0.5
                    width: 1
                    height: track.height
                    color: Theme.alpha(Theme.palette.m3Surface, 0.45)
                }
            }
        }
    }
}
