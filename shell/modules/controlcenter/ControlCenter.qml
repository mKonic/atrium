pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Effects
import Quickshell
import Quickshell.Bluetooth
import Quickshell.Networking
import Quickshell.Services.Mpris
import Quickshell.Services.Pipewire
import Quickshell.Wayland
import qs.components
import qs.services
import Atrium

// Control Center: the switches and sliders people reach for, one click from
// the bar. Wi-Fi and Bluetooth open their lists in place.
PanelWindow {
    id: cc

    property string page: ""  // "", "wifi", "bluetooth"

    readonly property var wifi: Networking.devices.values.find(d => d.type === DeviceType.Wifi) ?? null
    readonly property var wifiNetwork: wifi?.networks.values.find(n => n.connected) ?? null
    readonly property var adapter: Bluetooth.defaultAdapter
    readonly property var btConnected: adapter?.devices.values.filter(d => d.connected) ?? []
    readonly property PwNode sink: Pipewire.defaultAudioSink
    readonly property var player: Mpris.players.values.find(p => p.isPlaying) ?? Mpris.players.values[0] ?? null
    readonly property bool dnd: Atrium.settings["notifications.dnd"] ?? false
    readonly property string profile: Atrium.settings["power.profile"] ?? "performance"

    visible: Panels.open === "control"
    onVisibleChanged: page = ""
    screen: Quickshell.screens.find(s => s.name === Atrium.focusedOutput?.name) ?? Quickshell.screens[0]
    anchors {
        top: true
        right: true
    }
    margins {
        top: 8
        right: 8
    }
    implicitWidth: 400
    implicitHeight: panel.implicitHeight
    exclusiveZone: 0
    color: "transparent"
    WlrLayershell.layer: WlrLayer.Top
    WlrLayershell.namespace: "atrium-control-center"
    WlrLayershell.keyboardFocus: visible ? WlrKeyboardFocus.OnDemand : WlrKeyboardFocus.None

    PwObjectTracker {
        objects: [cc.sink]
    }

    Connections {
        target: Atrium

        function onFocusedWindowChanged(): void {
            // A window taking focus means a click elsewhere; the panel taking the
            // keyboard itself leaves no window focused and must not close it.
            if (!Atrium.focusedWindow)
                return;
            if (Panels.open === "control")
                Panels.open = "";
        }
    }

    function formatTime(s: int): string {
        return `${Math.floor(s / 60)}:${String(s % 60).padStart(2, "0")}`;
    }

    readonly property var profiles: ({
            "performance": { next: "balanced", icon: "bolt", name: "Performance" },
            "balanced": { next: "power-saver", icon: "balance", name: "Balanced" },
            "power-saver": { next: "performance", icon: "eco", name: "Power Saver" }
        })

    Rectangle {
        id: panel

        width: parent.width
        implicitHeight: (cc.page === "" ? main.implicitHeight : list.implicitHeight) + 24
        radius: 24
        color: Theme.panel(Theme.palette.m3Surface, 0.82)
        border.width: 1
        border.color: Theme.alpha(Theme.palette.m3Outline, 0.2)
        focus: true
        Keys.onEscapePressed: cc.page === "" ? Panels.open = "" : cc.page = ""

        Behavior on implicitHeight {
            Anim {
                duration: Theme.anim.small
            }
        }

        // --- the main page --------------------------------------------------
        Column {
            id: main

            x: 12
            y: 12
            width: parent.width - 24
            spacing: 10
            visible: cc.page === ""

            Row {
                width: parent.width
                spacing: 10

                Tile {
                    width: (parent.width - 10) / 2
                    wide: true
                    icon: !Networking.wifiEnabled ? "wifi_off" : cc.wifiNetwork ? "wifi" : "wifi_find"
                    title: "Wi-Fi"
                    subtitle: !cc.wifi ? "No Wi-Fi" : !Networking.wifiEnabled ? "Off" : cc.wifiNetwork?.name ?? "Not connected"
                    on: Networking.wifiEnabled && !!cc.wifiNetwork
                    expandable: !!cc.wifi
                    onToggled: Networking.wifiEnabled = !Networking.wifiEnabled
                    onExpand: cc.page = "wifi"
                }

                Tile {
                    width: (parent.width - 10) / 2
                    wide: true
                    icon: !cc.adapter?.enabled ? "bluetooth_disabled" : cc.btConnected.length ? "bluetooth_connected" : "bluetooth"
                    title: "Bluetooth"
                    subtitle: !cc.adapter ? "No Bluetooth" : !cc.adapter.enabled ? "Off"
                            : cc.btConnected.length ? cc.btConnected.map(d => d.name).join(", ") : "On"
                    on: cc.adapter?.enabled ?? false
                    expandable: !!cc.adapter
                    onToggled: if (cc.adapter) cc.adapter.enabled = !cc.adapter.enabled
                    onExpand: cc.page = "bluetooth"
                }
            }

            Row {
                width: parent.width
                spacing: 10

                Tile {
                    width: (parent.width - 20) / 3
                    compact: true
                    icon: cc.dnd ? "do_not_disturb_on" : "do_not_disturb_off"
                    title: "Do Not Disturb"
                    subtitle: cc.dnd ? "On" : "Off"
                    on: cc.dnd
                    onToggled: Atrium.setSetting("notifications.dnd", !cc.dnd)
                }

                Tile {
                    width: (parent.width - 20) / 3
                    compact: true
                    icon: cc.profiles[cc.profile]?.icon ?? "bolt"
                    title: "Power"
                    subtitle: cc.profiles[cc.profile]?.name ?? cc.profile
                    on: cc.profile === "performance"
                    onToggled: Atrium.setSetting("power.profile", cc.profiles[cc.profile]?.next ?? "performance")
                }

                Tile {
                    width: (parent.width - 20) / 3
                    compact: true
                    icon: Recorder.recording ? "stop_circle" : "screen_record"
                    title: "Record"
                    subtitle: Recorder.recording ? cc.formatTime(Recorder.seconds) : Recorder.available ? "Screen" : "Unavailable"
                    on: Recorder.recording
                    onToggled: {
                        if (Recorder.recording) {
                            Recorder.stop();
                        } else if (Recorder.available) {
                            const out = Atrium.focusedOutput;
                            Panels.open = "";
                            Recorder.start(out?.name ?? "", Math.round(out?.refresh ?? 60), Atrium.settings["recording.audio"] ?? false);
                        }
                    }
                }
            }

            // Sliders.
            Column {
                width: parent.width
                spacing: 6
                topPadding: 4
                visible: Brightness.available

                StyledText {
                    text: "Display"
                    font.pointSize: Theme.font.size.smaller
                    font.weight: Font.DemiBold
                    leftPadding: 4
                }

                BigSlider {
                    width: parent.width
                    icon: "brightness_6"
                    value: Brightness.value / 100
                    onMoved: v => Brightness.set(Math.round(v * 100))
                    onReleased: v => Atrium.setSetting("displays.brightness", Math.round(v * 100))
                }
            }

            Column {
                width: parent.width
                spacing: 6
                visible: !!cc.sink

                StyledText {
                    text: "Sound"
                    font.pointSize: Theme.font.size.smaller
                    font.weight: Font.DemiBold
                    leftPadding: 4
                }

                BigSlider {
                    width: parent.width
                    icon: cc.sink?.audio?.muted ? "volume_off" : "volume_up"
                    value: cc.sink?.audio?.muted ? 0 : cc.sink?.audio?.volume ?? 0
                    onMoved: v => {
                        if (!cc.sink?.audio)
                            return;
                        cc.sink.audio.muted = false;
                        cc.sink.audio.volume = v;
                    }
                }

                StyledText {
                    width: parent.width
                    leftPadding: 4
                    text: cc.sink?.description ?? ""
                    elide: Text.ElideRight
                    font.pointSize: Theme.font.size.small
                    color: Theme.palette.m3OnSurfaceVariant
                }
            }

            // Now playing.
            Rectangle {
                visible: !!cc.player
                width: parent.width
                height: 72
                radius: 18
                color: Theme.palette.m3SurfaceContainerHigh

                Rectangle {
                    id: art

                    anchors.left: parent.left
                    anchors.leftMargin: 10
                    anchors.verticalCenter: parent.verticalCenter
                    width: 52
                    height: 52
                    radius: 12
                    color: Theme.palette.m3SurfaceContainer
                    clip: true

                    Image {
                        anchors.fill: parent
                        source: cc.player?.trackArtUrl ?? ""
                        fillMode: Image.PreserveAspectCrop
                        asynchronous: true
                        sourceSize.width: 104
                        sourceSize.height: 104
                    }

                    MaterialIcon {
                        anchors.centerIn: parent
                        visible: !(cc.player?.trackArtUrl)
                        text: "music_note"
                        color: Theme.palette.m3OnSurfaceVariant
                    }
                }

                Column {
                    anchors.left: art.right
                    anchors.leftMargin: 10
                    anchors.right: controls.left
                    anchors.rightMargin: 8
                    anchors.verticalCenter: parent.verticalCenter

                    StyledText {
                        width: parent.width
                        text: cc.player?.trackTitle || cc.player?.identity || ""
                        elide: Text.ElideRight
                        font.weight: Font.DemiBold
                        font.pointSize: Theme.font.size.smaller
                    }

                    StyledText {
                        width: parent.width
                        text: cc.player?.trackArtist ?? ""
                        elide: Text.ElideRight
                        font.pointSize: Theme.font.size.small
                        color: Theme.palette.m3OnSurfaceVariant
                    }
                }

                Row {
                    id: controls

                    anchors.right: parent.right
                    anchors.rightMargin: 8
                    anchors.verticalCenter: parent.verticalCenter

                    Repeater {
                        model: [
                            { icon: "skip_previous", run: () => cc.player?.previous(), ok: cc.player?.canGoPrevious ?? false },
                            { icon: cc.player?.isPlaying ? "pause" : "play_arrow", run: () => cc.player?.togglePlaying(), ok: true },
                            { icon: "skip_next", run: () => cc.player?.next(), ok: cc.player?.canGoNext ?? false }
                        ]

                        Rectangle {
                            id: mediaButton

                            required property var modelData

                            width: 34
                            height: 34
                            radius: 17
                            color: mediaArea.containsMouse ? Theme.alpha(Theme.palette.m3OnSurface, 0.1) : "transparent"
                            opacity: modelData.ok ? 1 : 0.35

                            MaterialIcon {
                                anchors.centerIn: parent
                                text: mediaButton.modelData.icon
                                fill: 1
                                font.pointSize: Theme.font.size.larger
                            }

                            MouseArea {
                                id: mediaArea

                                anchors.fill: parent
                                hoverEnabled: true
                                enabled: mediaButton.modelData.ok
                                onClicked: mediaButton.modelData.run()
                            }
                        }
                    }
                }
            }
        }

        // --- Wi-Fi networks / Bluetooth devices -------------------------------
        Column {
            id: list

            x: 12
            y: 12
            width: parent.width - 24
            spacing: 6
            visible: cc.page !== ""

            Item {
                width: parent.width
                height: 36

                Rectangle {
                    id: backButton

                    anchors.left: parent.left
                    anchors.verticalCenter: parent.verticalCenter
                    width: 32
                    height: 32
                    radius: 16
                    color: backArea.containsMouse ? Theme.palette.m3SurfaceContainerHigh : "transparent"

                    MaterialIcon {
                        anchors.centerIn: parent
                        text: "arrow_back"
                    }

                    MouseArea {
                        id: backArea

                        anchors.fill: parent
                        hoverEnabled: true
                        onClicked: cc.page = ""
                    }
                }

                StyledText {
                    anchors.left: backButton.right
                    anchors.leftMargin: 8
                    anchors.verticalCenter: parent.verticalCenter
                    text: cc.page === "wifi" ? "Wi-Fi" : "Bluetooth"
                    font.pointSize: Theme.font.size.larger
                    font.weight: Font.DemiBold
                }
            }

            Repeater {
                model: cc.page === "wifi" ? (cc.wifi?.networks.values ?? []).slice().sort((a, b) => b.connected - a.connected || b.signalStrength - a.signalStrength).slice(0, 12)
                     : cc.page === "bluetooth" ? (cc.adapter?.devices.values ?? []).filter(d => d.paired || d.connected) : []

                Rectangle {
                    id: entry

                    required property var modelData
                    readonly property bool isWifi: cc.page === "wifi"

                    width: list.width
                    height: 44
                    radius: 12
                    color: entry.modelData.connected ? Theme.alpha(Theme.palette.m3Primary, 0.2)
                         : entryArea.containsMouse ? Theme.palette.m3SurfaceContainerHigh : "transparent"

                    MaterialIcon {
                        id: entryIcon

                        anchors.left: parent.left
                        anchors.leftMargin: 12
                        anchors.verticalCenter: parent.verticalCenter
                        text: entry.isWifi ? (entry.modelData.signalStrength > 0.66 ? "network_wifi" : entry.modelData.signalStrength > 0.33 ? "network_wifi_2_bar" : "network_wifi_1_bar")
                                           : (entry.modelData.icon?.includes("headset") || entry.modelData.icon?.includes("audio") ? "headphones" : "bluetooth")
                        fill: entry.modelData.connected ? 1 : 0
                    }

                    StyledText {
                        anchors.left: entryIcon.right
                        anchors.leftMargin: 10
                        anchors.right: status.left
                        anchors.rightMargin: 8
                        anchors.verticalCenter: parent.verticalCenter
                        text: entry.modelData.name || entry.modelData.deviceName || "Unknown"
                        elide: Text.ElideRight
                        font.weight: entry.modelData.connected ? Font.DemiBold : Font.Normal
                    }

                    StyledText {
                        id: status

                        anchors.right: parent.right
                        anchors.rightMargin: 12
                        anchors.verticalCenter: parent.verticalCenter
                        text: entry.modelData.connected ? "Connected"
                            : entry.isWifi ? (entry.modelData.known ? "" : entry.modelData.security > 0 ? "Secured" : "Open")
                            : entry.modelData.batteryAvailable ? `${Math.round(entry.modelData.battery * 100)}%` : ""
                        font.pointSize: Theme.font.size.small
                        color: Theme.palette.m3OnSurfaceVariant
                    }

                    MouseArea {
                        id: entryArea

                        anchors.fill: parent
                        hoverEnabled: true
                        onClicked: {
                            const d = entry.modelData;
                            if (entry.isWifi) {
                                if (d.connected)
                                    d.disconnect();
                                else if (d.known || d.security === 0)
                                    d.connect();
                                else
                                    Quickshell.execDetached(["nm-connection-editor"]);  // a password is needed
                            } else {
                                d.connected ? d.disconnect() : d.connect();
                            }
                        }
                    }
                }
            }

            StyledText {
                visible: cc.page === "bluetooth" && !(cc.adapter?.devices.values ?? []).some(d => d.paired || d.connected)
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                topPadding: 12
                bottomPadding: 12
                text: "No paired devices"
                color: Theme.palette.m3OnSurfaceVariant
            }
        }
    }
}
