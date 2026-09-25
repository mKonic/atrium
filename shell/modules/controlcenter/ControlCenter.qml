pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Effects
import Atrium.Shell
import Atrium
import shell.components
import shell.services

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

    readonly property WifiNetwork wifiNetwork: Network.current
    readonly property BluetoothAdapter adapter: Bluetooth.adapter
    readonly property AudioNode sink: Audio.sink
    readonly property var player: Mpris.current
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

    // Looking for Wi-Fi networks only while their list is open.
    Binding {
        when: cc.page === "wifi" && Network.wifiEnabled
        target: Network
        property: "scanning"
        value: true
        restoreMode: Binding.RestoreValue
    }

    // Looking for new Bluetooth devices only while their list is open.
    Binding {
        when: cc.page === "bluetooth" && cc.showOthers && (cc.adapter?.enabled ?? false)
        target: cc.adapter
        property: "discovering"
        value: true
        restoreMode: Binding.RestoreValue
    }
    screen: Shell.screen(Atrium.focusedOutput?.name)
    anchors {
        top: true
        right: true
    }
    margins {
        top: 8
        right: 8
    }
    implicitWidth: 340
    implicitHeight: panel.implicitHeight
    exclusiveZone: 0
    color: "transparent"
    WlrLayershell.layer: WlrLayer.Top
    WlrLayershell.namespace: "atrium-control-center"
    WlrLayershell.keyboardFocus: visible ? WlrKeyboardFocus.OnDemand : WlrKeyboardFocus.None

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
        radius: 30
        // No panel behind the main page's pieces of glass; an opened module
        // brings one in as it grows.
        color: "transparent"

        Rectangle {
            anchors.fill: parent
            radius: parent.radius
            color: Theme.material.regular
            border.width: Theme.lens ? 0 : 1
            border.color: Theme.palette.separator
            opacity: cc.open
            visible: opacity > 0.01

            Glass {}
        }
        focus: true
        Keys.onEscapePressed: cc.page === "" ? Panels.open = "" : cc.collapse()

        // --- the main page --------------------------------------------------
        // macOS 26's Control Center: each control its own piece of glass on
        // a four-column grid, floating over the desktop with no panel.
        Item {
            id: main

            readonly property real gap: 12
            readonly property real cell: (width - 3 * gap) / 4
            readonly property real span2: 2 * cell + gap
            readonly property bool playing: !!cc.player

            x: 12
            y: 12
            width: parent.width - 24
            implicitHeight: sliders.y + sliders.height
            // Recedes as the page grows over it.
            opacity: 1 - Math.min(1, cc.open * 1.8)
            scale: 1 - 0.04 * cc.open
            visible: opacity > 0
            enabled: cc.page === ""

            CapsuleToggle {
                id: wifiTile

                width: main.span2
                height: main.cell
                icon: !Network.wifiEnabled ? "wifi_off" : cc.wifiNetwork ? "wifi" : "wifi_find"
                title: "Wi-Fi"
                subtitle: !Network.hasWifi ? "No Wi-Fi" : !Network.wifiEnabled ? "Off" : cc.wifiNetwork?.name ?? "Not connected"
                on: Network.wifiEnabled
                expandable: Network.hasWifi
                onToggled: Network.wifiEnabled = !Network.wifiEnabled
                onExpand: cc.expand("wifi", wifiTile)
            }

            CapsuleToggle {
                id: btTile

                y: main.cell + main.gap
                width: main.span2
                height: main.cell
                icon: !cc.adapter?.enabled ? "bluetooth_disabled" : cc.adapter.connectedNames ? "bluetooth_connected" : "bluetooth"
                title: "Bluetooth"
                subtitle: !cc.adapter ? "No Bluetooth" : !cc.adapter.enabled ? "Off" : cc.adapter.connectedNames || "On"
                on: cc.adapter?.enabled ?? false
                expandable: !!cc.adapter
                onToggled: if (cc.adapter) cc.adapter.enabled = !cc.adapter.enabled
                onExpand: cc.expand("bluetooth", btTile)
            }

            // Top right: what's playing, a 2x2 square; with nothing playing
            // four round buttons take its place (and move down a row
            // otherwise).
            Grid {
                id: rounds

                x: main.playing ? 0 : main.span2 + main.gap
                y: main.playing ? 2 * (main.cell + main.gap) : 0
                columns: main.playing ? 4 : 2
                spacing: main.gap

                CircleToggle {
                    width: main.cell
                    height: main.cell
                    icon: cc.dnd ? "do_not_disturb_on" : "do_not_disturb_off"
                    title: cc.dnd ? "Do Not Disturb: On" : "Do Not Disturb"
                    on: cc.dnd
                    onToggled: Atrium.setSetting("notifications.dnd", !cc.dnd)
                }

                CircleToggle {
                    width: main.cell
                    height: main.cell
                    icon: "nightlight"
                    title: Atrium.nightLight.note || ((Atrium.nightLight.active ?? false) ? "Night Light: On" : "Night Light")
                    on: Atrium.nightLight.active ?? false
                    onToggled: Atrium.setNightLight(!(Atrium.nightLight.active ?? false))
                }

                CircleToggle {
                    width: main.cell
                    height: main.cell
                    icon: Recorder.recording ? "stop_circle" : "screen_record"
                    title: Recorder.recording ? `Recording ${cc.formatTime(Recorder.seconds)}` : Recorder.available ? "Record Screen" : "Recording unavailable"
                    on: Recorder.recording
                    onToggled: {
                        Panels.open = "";
                        Recorder.toggle();
                    }
                }

                CircleToggle {
                    width: main.cell
                    height: main.cell
                    icon: cc.profiles[cc.profile]?.icon ?? "bolt"
                    title: `Power: ${cc.profiles[cc.profile]?.name ?? cc.profile}`
                    on: cc.profile === "performance"
                    onToggled: Atrium.setSetting("power.profile", cc.profiles[cc.profile]?.next ?? "performance")
                }
            }

            Module {
                id: mediaTile

                visible: main.playing
                x: main.span2 + main.gap
                width: main.span2
                height: main.span2
                radius: 30

                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: cc.expand("media", mediaTile)
                }

                Rectangle {
                    id: art

                    x: 16
                    y: 16
                    width: 46
                    height: 46
                    radius: 11
                    color: Theme.palette.tertiaryFill
                    clip: true

                    Image {
                        anchors.fill: parent
                        source: cc.player?.trackArtUrl ?? ""
                        fillMode: Image.PreserveAspectCrop
                        asynchronous: true
                        sourceSize.width: 92
                        sourceSize.height: 92
                    }

                    MaterialIcon {
                        anchors.centerIn: parent
                        visible: !(cc.player?.trackArtUrl)
                        text: "music_note"
                        color: Theme.palette.secondaryLabel
                    }
                }

                Column {
                    x: 16
                    anchors.top: art.bottom
                    anchors.topMargin: 8
                    width: parent.width - 32

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
                        color: Theme.palette.secondaryLabel
                    }
                }

                Row {
                    anchors.horizontalCenter: parent.horizontalCenter
                    anchors.bottom: parent.bottom
                    anchors.bottomMargin: 10
                    spacing: 6

                    Repeater {
                        model: [
                            { icon: "fast_rewind", run: () => cc.player?.previous(), ok: cc.player?.canGoPrevious ?? false },
                            { icon: cc.player?.isPlaying ? "pause" : "play_arrow", run: () => cc.player?.togglePlaying(), ok: true },
                            { icon: "fast_forward", run: () => cc.player?.next(), ok: cc.player?.canGoNext ?? false }
                        ]

                        Rectangle {
                            id: mediaButton

                            required property var modelData

                            width: 34
                            height: 34
                            radius: 17
                            color: mediaArea.containsMouse ? Theme.palette.secondaryFill : "transparent"
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

            Column {
                id: sliders

                y: main.playing ? rounds.y + main.cell + main.gap : 2 * (main.cell + main.gap)
                width: parent.width
                spacing: main.gap

                SliderModule {
                    visible: Brightness.available
                    width: parent.width
                    title: "Display"
                    lowIcon: "brightness_low"
                    highIcon: "brightness_high"
                    value: Brightness.value / 100
                    onMoved: v => Brightness.set(Math.round(v * 100))
                    onReleased: v => Brightness.commit(Math.round(v * 100))
                }

                SliderModule {
                    visible: !!cc.sink
                    width: parent.width
                    title: cc.sink?.label ? `Sound · ${cc.sink.label}` : "Sound"
                    lowIcon: cc.sink?.muted ? "volume_off" : "volume_mute"
                    highIcon: "volume_up"
                    value: cc.sink?.muted ? 0 : cc.sink?.volume ?? 0
                    onMoved: v => {
                        if (!cc.sink)
                            return;
                        cc.sink.muted = false;
                        cc.sink.volume = v;
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
            readonly property color tileColor: cc.pageTile?.color ?? Theme.palette.tertiaryFill

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
                        checked: cc.page === "wifi" ? Network.wifiEnabled : (cc.adapter?.enabled ?? false)
                        onToggled: {
                            if (cc.page === "wifi")
                                Network.wifiEnabled = !Network.wifiEnabled;
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
                    players: Mpris.players
                    onChoose: p => Mpris.chosen = p
                }

                // --- Wi-Fi ---
                SectionLabel {
                    visible: cc.page === "wifi" && Network.wifiEnabled
                    text: "Known Networks"
                }

                Repeater {
                    model: cc.page === "wifi" && Network.wifiEnabled ? Network.saved : []

                    delegate: networkRow
                }

                Disclosure {
                    visible: cc.page === "wifi" && Network.wifiEnabled
                    text: "Other Networks"
                }

                Repeater {
                    model: cc.page === "wifi" && Network.wifiEnabled && cc.showOthers ? Network.unsaved : []

                    delegate: networkRow
                }

                // --- Bluetooth ---
                SectionLabel {
                    visible: cc.page === "bluetooth" && (cc.adapter?.enabled ?? false)
                    text: "Devices"
                }

                Repeater {
                    model: cc.page === "bluetooth" && (cc.adapter?.enabled ?? false) ? cc.adapter.mine : []

                    Entry {
                        required property var modelData

                        glyph: modelData.glyph
                        name: modelData.name || modelData.deviceName || "Unknown"
                        connected: modelData.connected
                        busy: modelData.state === BluetoothDeviceState.Connecting || modelData.state === BluetoothDeviceState.Disconnecting
                        status: modelData.connected && modelData.batteryAvailable ? `${Math.round(modelData.battery * 100)}%` : ""
                        onClicked: modelData.connected ? modelData.disconnect() : modelData.connect()
                    }
                }

                StyledText {
                    visible: cc.page === "bluetooth" && (cc.adapter?.enabled ?? false)
                             && cc.adapter.mine.length === 0
                    x: 14
                    topPadding: 4
                    bottomPadding: 8
                    text: "No devices"
                    color: Theme.palette.secondaryLabel
                }

                Disclosure {
                    visible: cc.page === "bluetooth" && (cc.adapter?.enabled ?? false)
                    text: "Other Devices"
                    note: cc.showOthers && cc.adapter?.discovering ? "Searching…" : ""
                }

                Repeater {
                    model: cc.page === "bluetooth" && (cc.adapter?.enabled ?? false) && cc.showOthers ? cc.adapter.nearby : []

                    Entry {
                        id: nearby

                        required property var modelData

                        glyph: modelData.glyph
                        name: modelData.name
                        busy: modelData.pairing
                        status: modelData.pairing ? "Pairing…" : ""
                        onClicked: modelData.pairing ? modelData.cancelPair() : modelData.pair()
                    }
                }

                Separator {}

                // Everything else about it, in System Settings.
                Rectangle {
                    width: list.width
                    height: 38
                    radius: 12
                    color: settingsArea.containsMouse ? Theme.palette.tertiaryFill : "transparent"

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
                    glyph: network.modelData.glyph
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
                        color: Theme.palette.tertiaryFill
                        border.width: 1
                        border.color: cc.joinError ? Theme.palette.red : password.activeFocus ? Theme.palette.focusRing : Theme.palette.separator

                        TextInput {
                            id: password

                            anchors.fill: parent
                            anchors.leftMargin: 12
                            anchors.rightMargin: 36
                            verticalAlignment: TextInput.AlignVCenter
                            echoMode: TextInput.Password
                            color: Theme.palette.label
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
                                color: cc.joinError ? Theme.palette.red : Theme.palette.tertiaryLabel
                                font.pointSize: Theme.font.size.small
                            }
                        }

                        MaterialIcon {
                            anchors.right: parent.right
                            anchors.rightMargin: 8
                            anchors.verticalCenter: parent.verticalCenter
                            text: "arrow_forward"
                            color: password.text ? Theme.palette.accent : Theme.palette.secondaryLabel

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
        color: entryArea.containsMouse ? Theme.palette.tertiaryFill : "transparent"

        Rectangle {
            id: badge

            anchors.left: parent.left
            anchors.leftMargin: 8
            anchors.verticalCenter: parent.verticalCenter
            width: 30
            height: 30
            radius: 15
            color: entry.connected ? Theme.palette.accent : Theme.palette.secondaryFill

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
                color: entry.connected ? Theme.palette.labelOnAccent : Theme.palette.label
            }

            // Working on it: a ring turning around the badge.
            Rectangle {
                anchors.centerIn: parent
                width: parent.width + 6
                height: width
                radius: width / 2
                color: "transparent"
                border.width: 2
                border.color: Theme.palette.focusRing
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
            color: Theme.palette.secondaryLabel
        }

        MaterialIcon {
            anchors.right: parent.right
            anchors.rightMargin: 12
            anchors.verticalCenter: parent.verticalCenter
            visible: entry.statusIsGlyph && entry.status
            text: entry.status
            font.pointSize: Theme.font.size.normal
            color: Theme.palette.secondaryLabel
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
            color: disclosureArea.containsMouse ? Theme.palette.tertiaryFill : "transparent"
        }

        StyledText {
            x: 14
            anchors.verticalCenter: parent.verticalCenter
            text: disclosure.text
            font.weight: Font.DemiBold
            color: Theme.palette.accent
        }

        StyledText {
            anchors.right: arrow.left
            anchors.rightMargin: 6
            anchors.verticalCenter: parent.verticalCenter
            text: disclosure.note
            font.pointSize: Theme.font.size.small
            color: Theme.palette.secondaryLabel
        }

        MaterialIcon {
            id: arrow

            anchors.right: parent.right
            anchors.rightMargin: 10
            anchors.verticalCenter: parent.verticalCenter
            text: "chevron_right"
            rotation: cc.showOthers ? 90 : 0
            color: Theme.palette.secondaryLabel

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
        color: Theme.palette.secondaryLabel
    }

    component Separator: Rectangle {
        x: 10
        width: list.width - 20
        height: 1
        color: Theme.palette.separator
    }
}
