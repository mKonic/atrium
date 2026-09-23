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

    property string page: ""  // "", "wifi", "bluetooth", "media"
    property Item pageTile: null  // the tile the page grew out of
    property real open: 0         // 0: the tiles, 1: the page, between: growing
    property var joining: null    // a Wi-Fi network asking for its password
    property string joinError: ""
    property bool showOthers: false  // "Other Networks" / "Other Devices" disclosed

    readonly property var wifi: Networking.devices.values.find(d => d.type === DeviceType.Wifi) ?? null
    readonly property var wifiNetwork: wifi?.networks.values.find(n => n.connected) ?? null
    readonly property var adapter: Bluetooth.defaultAdapter
    readonly property var btConnected: adapter?.devices.values.filter(d => d.connected) ?? []
    readonly property PwNode sink: Pipewire.defaultAudioSink
    property var chosenPlayer: null  // picked on the Now Playing page
    readonly property var player: (Mpris.players.values.includes(chosenPlayer) ? chosenPlayer : null)
                                  ?? Mpris.players.values.find(p => p.isPlaying) ?? Mpris.players.values[0] ?? null
    readonly property bool dnd: Atrium.settings["notifications.dnd"] ?? false
    readonly property string profile: Atrium.settings["power.profile"] ?? "performance"

    visible: Panels.open === "control"
    onVisibleChanged: {
        grow.stop();
        open = 0;
        page = "";
        joining = null;
    }

    // A module's tile grows into the whole panel, as in macOS Control Center,
    // and shrinks back into its place.
    function expand(name: string, tile: Item): void {
        page = name;
        pageTile = tile;
        joining = null;
        showOthers = false;
        grow.to = 1;
        grow.duration = Theme.anim.normal * 0.8;
        grow.restart();
    }

    function collapse(): void {
        joining = null;
        grow.to = 0;
        grow.duration = Theme.anim.small * 1.3;
        grow.restart();
    }

    Binding {
        target: Panels
        property: "deep"
        value: cc.page ? "control" : ""
    }

    Connections {
        target: Panels

        function onBack(name: string): void {
            if (name === "control")
                cc.collapse();
        }
    }

    NumberAnimation {
        id: grow

        target: cc
        property: "open"
        easing.type: Easing.BezierSpline
        easing.bezierCurve: Theme.anim.emphasizedDecel
        onFinished: if (cc.open === 0) cc.page = ""
    }

    // Looking for new Bluetooth devices only while their list is open.
    Binding {
        when: cc.page === "bluetooth" && cc.showOthers && (cc.adapter?.enabled ?? false)
        target: cc.adapter
        property: "discovering"
        value: true
        restoreMode: Binding.RestoreValue
    }
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
        implicitHeight: main.implicitHeight + (list.implicitHeight - main.implicitHeight) * cc.open + 24
        radius: 24
        color: Theme.panel(Theme.palette.m3Surface, 0.82)
        border.width: 1
        border.color: Theme.alpha(Theme.palette.m3Outline, 0.2)
        focus: true
        Keys.onEscapePressed: cc.page === "" ? Panels.open = "" : cc.collapse()

        // --- the main page --------------------------------------------------
        Column {
            id: main

            x: 12
            y: 12
            width: parent.width - 24
            spacing: 10
            // Recedes as the page grows over it.
            opacity: 1 - Math.min(1, cc.open * 1.8)
            scale: 1 - 0.04 * cc.open
            visible: opacity > 0
            enabled: cc.page === ""

            Row {
                width: parent.width
                spacing: 10

                Tile {
                    id: wifiTile

                    width: (parent.width - 10) / 2
                    wide: true
                    icon: !Networking.wifiEnabled ? "wifi_off" : cc.wifiNetwork ? "wifi" : "wifi_find"
                    title: "Wi-Fi"
                    subtitle: !cc.wifi ? "No Wi-Fi" : !Networking.wifiEnabled ? "Off" : cc.wifiNetwork?.name ?? "Not connected"
                    on: Networking.wifiEnabled && !!cc.wifiNetwork
                    expandable: !!cc.wifi
                    onToggled: Networking.wifiEnabled = !Networking.wifiEnabled
                    onExpand: cc.expand("wifi", wifiTile)
                }

                Tile {
                    id: btTile

                    width: (parent.width - 10) / 2
                    wide: true
                    icon: !cc.adapter?.enabled ? "bluetooth_disabled" : cc.btConnected.length ? "bluetooth_connected" : "bluetooth"
                    title: "Bluetooth"
                    subtitle: !cc.adapter ? "No Bluetooth" : !cc.adapter.enabled ? "Off"
                            : cc.btConnected.length ? cc.btConnected.map(d => d.name).join(", ") : "On"
                    on: cc.adapter?.enabled ?? false
                    expandable: !!cc.adapter
                    onToggled: if (cc.adapter) cc.adapter.enabled = !cc.adapter.enabled
                    onExpand: cc.expand("bluetooth", btTile)
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

            // Now playing: grows into the Now Playing page when clicked.
            Rectangle {
                id: mediaTile

                visible: !!cc.player
                width: parent.width
                height: 72
                radius: 18
                color: Theme.palette.m3SurfaceContainerHigh

                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: cc.expand("media", mediaTile)
                }

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

        // --- a module, grown out of its tile into the whole panel -------------
        Rectangle {
            id: sheet

            // Where the tile sits, when the page starts growing out of it.
            readonly property rect from: cc.pageTile ? cc.pageTile.mapToItem(panel, 0, 0, cc.pageTile.width, cc.pageTile.height)
                                                     : Qt.rect(12, 12, panel.width - 24, 64)
            readonly property color tileColor: cc.pageTile?.color ?? Theme.palette.m3SurfaceContainerHigh

            visible: cc.open > 0
            x: from.x + (12 - from.x) * cc.open
            y: from.y + (12 - from.y) * cc.open
            width: from.width + (panel.width - 24 - from.width) * cc.open
            height: from.height + (list.implicitHeight - from.height) * cc.open
            radius: 18
            // The tile's own colour, melting into the panel.
            color: Qt.rgba(tileColor.r, tileColor.g, tileColor.b, tileColor.a * (1 - cc.open))
            clip: true

            Column {
                id: list

                width: panel.width - 24
                opacity: Math.max(0, (cc.open - 0.35) / 0.65)
                spacing: 2

                // Title and the module's own switch.
                Item {
                    width: parent.width
                    height: 44

                    MouseArea {
                        id: back

                        anchors.left: parent.left
                        anchors.top: parent.top
                        anchors.bottom: parent.bottom
                        width: title.x + title.width + 8
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: cc.collapse()
                    }

                    StyledText {
                        id: title

                        x: 14
                        anchors.verticalCenter: parent.verticalCenter
                        text: cc.page === "wifi" ? "Wi-Fi" : cc.page === "media" ? "Now Playing" : "Bluetooth"
                        font.pointSize: Theme.font.size.larger
                        font.weight: Font.DemiBold
                    }

                    Switch {
                        visible: cc.page !== "media"
                        anchors.right: parent.right
                        anchors.rightMargin: 10
                        anchors.verticalCenter: parent.verticalCenter
                        checked: cc.page === "wifi" ? Networking.wifiEnabled : (cc.adapter?.enabled ?? false)
                        onToggled: {
                            if (cc.page === "wifi")
                                Networking.wifiEnabled = !Networking.wifiEnabled;
                            else if (cc.adapter)
                                cc.adapter.enabled = !cc.adapter.enabled;
                        }
                    }
                }

                Separator {}

                // --- Now Playing ---
                NowPlaying {
                    visible: cc.page === "media"
                    width: list.width
                    player: cc.player
                    players: Mpris.players.values
                    onChoose: p => cc.chosenPlayer = p
                }

                // --- Wi-Fi ---
                SectionLabel {
                    visible: cc.page === "wifi" && Networking.wifiEnabled
                    text: "Known Networks"
                }

                Repeater {
                    model: cc.page === "wifi" && Networking.wifiEnabled ? cc.networks(true) : []

                    delegate: networkRow
                }

                Disclosure {
                    visible: cc.page === "wifi" && Networking.wifiEnabled
                    text: "Other Networks"
                }

                Repeater {
                    model: cc.page === "wifi" && Networking.wifiEnabled && cc.showOthers ? cc.networks(false) : []

                    delegate: networkRow
                }

                // --- Bluetooth ---
                SectionLabel {
                    visible: cc.page === "bluetooth" && (cc.adapter?.enabled ?? false)
                    text: "Devices"
                }

                Repeater {
                    model: cc.page === "bluetooth" && (cc.adapter?.enabled ?? false)
                           ? (cc.adapter?.devices.values ?? []).filter(d => d.paired || d.connected) : []

                    Entry {
                        required property var modelData

                        glyph: cc.deviceGlyph(modelData)
                        name: modelData.name || modelData.deviceName || "Unknown"
                        connected: modelData.connected
                        busy: modelData.state === BluetoothDeviceState.Connecting || modelData.state === BluetoothDeviceState.Disconnecting
                        status: modelData.connected && modelData.batteryAvailable ? `${Math.round(modelData.battery * 100)}%` : ""
                        onClicked: modelData.connected ? modelData.disconnect() : modelData.connect()
                    }
                }

                StyledText {
                    visible: cc.page === "bluetooth" && (cc.adapter?.enabled ?? false)
                             && !(cc.adapter?.devices.values ?? []).some(d => d.paired || d.connected)
                    x: 14
                    topPadding: 4
                    bottomPadding: 8
                    text: "No devices"
                    color: Theme.palette.m3OnSurfaceVariant
                }

                Disclosure {
                    visible: cc.page === "bluetooth" && (cc.adapter?.enabled ?? false)
                    text: "Other Devices"
                    note: cc.showOthers && cc.adapter?.discovering ? "Searching…" : ""
                }

                Repeater {
                    model: cc.page === "bluetooth" && (cc.adapter?.enabled ?? false) && cc.showOthers
                           ? (cc.adapter?.devices.values ?? []).filter(d => !d.paired && !d.connected && d.name && d.name !== d.address.replace(/:/g, "-")).slice(0, 8) : []

                    Entry {
                        id: nearby

                        required property var modelData

                        glyph: cc.deviceGlyph(modelData)
                        name: modelData.name
                        busy: modelData.pairing
                        status: modelData.pairing ? "Pairing…" : ""
                        onClicked: modelData.pairing ? modelData.cancelPair() : modelData.pair()

                        // Paired: trust it, so it reconnects by itself, and connect.
                        Connections {
                            target: nearby.modelData

                            function onPairedChanged(): void {
                                if (!nearby.modelData.paired)
                                    return;
                                nearby.modelData.trusted = true;
                                nearby.modelData.connect();
                            }
                        }
                    }
                }

                Separator {}

                // Everything else about it, in System Settings.
                Rectangle {
                    width: list.width
                    height: 38
                    radius: 12
                    color: settingsArea.containsMouse ? Theme.alpha(Theme.palette.m3OnSurface, 0.07) : "transparent"

                    StyledText {
                        x: 14
                        anchors.verticalCenter: parent.verticalCenter
                        text: cc.page === "wifi" ? "Wi-Fi Settings…" : cc.page === "media" ? "Sound Settings…" : "Bluetooth Settings…"
                    }

                    MouseArea {
                        id: settingsArea

                        anchors.fill: parent
                        hoverEnabled: true
                        onClicked: {
                            Atrium.action("shell", cc.page === "wifi" ? "settings:Wi-Fi & Network"
                                                 : cc.page === "media" ? "settings:Sound" : "settings:Bluetooth");
                            Panels.open = "";
                        }
                    }
                }

                Item {
                    width: 1
                    height: 6
                }
            }
        }
    }

    Component {
        id: networkRow

            Column {
                id: network

                required property var modelData
                readonly property bool asking: cc.joining === modelData

                width: list.width

                Entry {
                    glyph: network.modelData.signalStrength > 0.66 ? "network_wifi" : network.modelData.signalStrength > 0.33 ? "network_wifi_2_bar" : "network_wifi_1_bar"
                    name: network.modelData.name
                    connected: network.modelData.connected
                    busy: network.modelData.stateChanging
                    status: network.modelData.security > 0 ? "lock" : ""
                    statusIsGlyph: true
                    onClicked: {
                        const n = network.modelData;
                        if (n.connected)
                            n.disconnect();
                        else if (n.known || n.security === 0)
                            n.connect();
                        else {
                            cc.joinError = "";
                            cc.joining = network.asking ? null : n;
                        }
                    }
                }

                // A secured network asks for its password right here.
                Item {
                    visible: network.asking
                    width: parent.width
                    height: visible ? 52 : 0

                    Rectangle {
                        anchors.fill: parent
                        anchors.leftMargin: 48
                        anchors.rightMargin: 10
                        anchors.topMargin: 4
                        anchors.bottomMargin: 8
                        radius: 10
                        color: Theme.palette.m3SurfaceContainerHigh
                        border.width: 1
                        border.color: cc.joinError ? "#ffb4ab" : Theme.alpha(Theme.palette.m3Primary, password.activeFocus ? 0.8 : 0.3)

                        TextInput {
                            id: password

                            anchors.fill: parent
                            anchors.leftMargin: 12
                            anchors.rightMargin: 36
                            verticalAlignment: TextInput.AlignVCenter
                            echoMode: TextInput.Password
                            color: Theme.palette.m3OnSurface
                            font.family: Theme.font.sans
                            font.pointSize: Theme.font.size.normal
                            clip: true
                            onAccepted: {
                                if (!text)
                                    return;
                                cc.joinError = "";
                                network.modelData.connectWithPsk(text);
                            }
                            Keys.onEscapePressed: cc.joining = null

                            StyledText {
                                anchors.verticalCenter: parent.verticalCenter
                                visible: !password.text
                                text: cc.joinError || "Password"
                                color: cc.joinError ? "#ffb4ab" : Theme.alpha(Theme.palette.m3OnSurfaceVariant, 0.7)
                                font.pointSize: Theme.font.size.small
                            }
                        }

                        MaterialIcon {
                            anchors.right: parent.right
                            anchors.rightMargin: 8
                            anchors.verticalCenter: parent.verticalCenter
                            text: "arrow_forward"
                            color: password.text ? Theme.palette.m3Primary : Theme.palette.m3OnSurfaceVariant

                            MouseArea {
                                anchors.fill: parent
                                anchors.margins: -6
                                onClicked: password.accepted()
                            }
                        }
                    }

                    onVisibleChanged: {
                        password.text = "";
                        if (visible)
                            password.forceActiveFocus();
                    }
                }

                Connections {
                    target: network.asking ? network.modelData : null

                    function onConnectionFailed(reason: var): void {
                        cc.joinError = "Wrong password";
                    }

                    function onConnectedChanged(): void {
                        if (network.modelData.connected)
                            cc.joining = null;
                    }
                }
            }
    }

    // Known networks (or the rest), connected first, strongest next.
    function networks(known: bool): var {
        return (wifi?.networks.values ?? []).filter(n => (n.known || n.connected) === known)
            .sort((a, b) => b.connected - a.connected || b.signalStrength - a.signalStrength).slice(0, 12);
    }

    function deviceGlyph(d: var): string {
        const icon = d?.icon ?? "";
        return icon.includes("headset") || icon.includes("headphone") || icon.includes("audio") ? "headphones"
             : icon.includes("phone") ? "smartphone"
             : icon.includes("keyboard") ? "keyboard"
             : icon.includes("mouse") ? "mouse"
             : icon.includes("gaming") || icon.includes("joystick") ? "sports_esports"
             : icon.includes("computer") ? "computer"
             : "bluetooth";
    }

    // A row of a module's list: a round badge (filled while connected), the
    // name, and a note or glyph at the end.
    component Entry: Rectangle {
        id: entry

        property string glyph
        property string name
        property string status
        property bool statusIsGlyph: false
        property bool connected: false
        property bool busy: false
        signal clicked

        width: list.width
        height: 44
        radius: 12
        color: entryArea.containsMouse ? Theme.alpha(Theme.palette.m3OnSurface, 0.07) : "transparent"

        Rectangle {
            id: badge

            anchors.left: parent.left
            anchors.leftMargin: 8
            anchors.verticalCenter: parent.verticalCenter
            width: 30
            height: 30
            radius: 15
            color: entry.connected ? Theme.palette.m3Primary : Theme.alpha(Theme.palette.m3OnSurface, 0.12)

            Behavior on color {
                CAnim {
                    duration: Theme.anim.small
                }
            }

            MaterialIcon {
                anchors.centerIn: parent
                text: entry.glyph
                fill: entry.connected ? 1 : 0
                font.pointSize: Theme.font.size.normal
                color: entry.connected ? Theme.palette.m3OnPrimary : Theme.palette.m3OnSurface
            }

            // Working on it: a ring turning around the badge.
            Rectangle {
                anchors.centerIn: parent
                width: parent.width + 6
                height: width
                radius: width / 2
                color: "transparent"
                border.width: 2
                border.color: Theme.alpha(Theme.palette.m3Primary, 0.6)
                visible: entry.busy
                opacity: 0.4

                SequentialAnimation on opacity {
                    running: entry.busy
                    loops: Animation.Infinite

                    Anim {
                        to: 1
                        duration: 500
                    }
                    Anim {
                        to: 0.3
                        duration: 500
                    }
                }
            }
        }

        StyledText {
            anchors.left: badge.right
            anchors.leftMargin: 12
            anchors.right: note.left
            anchors.rightMargin: 8
            anchors.verticalCenter: parent.verticalCenter
            text: entry.name
            elide: Text.ElideRight
            font.weight: entry.connected ? Font.DemiBold : Font.Normal
        }

        StyledText {
            id: note

            anchors.right: parent.right
            anchors.rightMargin: 12
            anchors.verticalCenter: parent.verticalCenter
            visible: !entry.statusIsGlyph
            text: entry.status
            font.pointSize: Theme.font.size.small
            color: Theme.palette.m3OnSurfaceVariant
        }

        MaterialIcon {
            anchors.right: parent.right
            anchors.rightMargin: 12
            anchors.verticalCenter: parent.verticalCenter
            visible: entry.statusIsGlyph && entry.status
            text: entry.status
            font.pointSize: Theme.font.size.normal
            color: Theme.palette.m3OnSurfaceVariant
        }

        MouseArea {
            id: entryArea

            anchors.fill: parent
            hoverEnabled: true
            onClicked: entry.clicked()
        }
    }

    // An accent-coloured row that shows or hides the rest of a list.
    component Disclosure: Item {
        id: disclosure

        property string text
        property string note

        width: list.width
        height: 36

        Rectangle {
            anchors.fill: parent
            radius: 12
            color: disclosureArea.containsMouse ? Theme.alpha(Theme.palette.m3OnSurface, 0.07) : "transparent"
        }

        StyledText {
            x: 14
            anchors.verticalCenter: parent.verticalCenter
            text: disclosure.text
            font.weight: Font.DemiBold
            color: Theme.palette.m3Primary
        }

        StyledText {
            anchors.right: arrow.left
            anchors.rightMargin: 6
            anchors.verticalCenter: parent.verticalCenter
            text: disclosure.note
            font.pointSize: Theme.font.size.small
            color: Theme.palette.m3OnSurfaceVariant
        }

        MaterialIcon {
            id: arrow

            anchors.right: parent.right
            anchors.rightMargin: 10
            anchors.verticalCenter: parent.verticalCenter
            text: "chevron_right"
            rotation: cc.showOthers ? 90 : 0
            color: Theme.palette.m3OnSurfaceVariant

            Behavior on rotation {
                Anim {
                    duration: Theme.anim.small
                }
            }
        }

        MouseArea {
            id: disclosureArea

            anchors.fill: parent
            hoverEnabled: true
            onClicked: cc.showOthers = !cc.showOthers
        }
    }

    component SectionLabel: StyledText {
        x: 14
        topPadding: 8
        bottomPadding: 4
        font.pointSize: Theme.font.size.smaller
        font.weight: Font.DemiBold
        color: Theme.palette.m3OnSurfaceVariant
    }

    component Separator: Rectangle {
        x: 10
        width: list.width - 20
        height: 1
        color: Theme.alpha(Theme.palette.m3Outline, 0.2)
    }
}
