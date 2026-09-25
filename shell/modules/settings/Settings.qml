pragma ComponentBehavior: Bound

import QtQuick
import Atrium.Shell
import shell.components
import shell.services
import Atrium

// System Settings: every setting atrium has, grouped into pages in a
// sidebar, as macOS lays them out. Pages are made from the settings schema;
// a few (About, shortcuts, window rules) have their own views.
FloatingWindow {
    id: root

    property string page: "Appearance"
    property string query: ""

    // Types that need a whole card rather than a control at the row's end.
    function wide(type: string): bool {
        return type === "list";
    }

    function openPage(name: string): void {
        if (name)
            page = name;
        query = "";
        search.text = "";
        visible = true;
        // Already open behind other windows: bring it forward.
        const self = Atrium.windows.find(w => w.app_id === "atrium-settings");
        if (self)
            Atrium.focusWindow(self.id);
    }

    title: "System Settings"
    visible: false
    // As macOS 26's Settings: no title bar; the traffic lights sit in the
    // sidebar, a pane inset in the window with a fine light rim.
    titleBar: false
    color: Theme.palette.windowBackground
    implicitWidth: 920
    implicitHeight: 660
    minimumSize: Qt.size(720, 480)

    Connections {
        target: Atrium

        function onShellAction(name: string): void {
            // "settings" or "settings:Page Name"
            if (name === "settings" || name.startsWith("settings:"))
                root.openPage(name.slice(9));
        }
    }

    // Empty window background moves the window, as a title bar would.
    MouseArea {
        anchors.fill: parent
        onPressed: root.startMove()
        onDoubleClicked: root.toggleZoom()
    }

    // --- sidebar -----------------------------------------------------------
    Rectangle {
        id: sidebar

        readonly property int inset: 8

        x: inset
        y: inset
        width: 240 - inset
        height: parent.height - 2 * inset
        radius: 16
        // A shade off the window, nearly opaque (Finder's is a few steps
        // darker in dark mode), and a rim that catches the light.
        color: Theme.light ? Qt.darker(Theme.palette.windowBackground, 1.035) : Qt.darker(Theme.palette.windowBackground, 1.16)
        border.width: 1
        border.color: Theme.light ? Qt.rgba(0, 0, 0, 0.09) : Qt.rgba(1, 1, 1, 0.14)

        MouseArea {
            anchors.fill: parent
            onPressed: root.startMove()
            onDoubleClicked: root.toggleZoom()
        }

        Rectangle {
            id: searchBox

            x: 12
            y: 14
            width: parent.width - 24
            height: 34
            radius: 10
            color: Theme.palette.tertiaryFill

            MaterialIcon {
                id: glass

                anchors.left: parent.left
                anchors.leftMargin: 10
                anchors.verticalCenter: parent.verticalCenter
                text: "search"
                font.pointSize: Theme.font.size.normal
                color: Theme.palette.secondaryLabel
            }

            TextInput {
                id: search

                anchors.left: glass.right
                anchors.leftMargin: 6
                anchors.right: parent.right
                anchors.rightMargin: 10
                anchors.verticalCenter: parent.verticalCenter
                color: Theme.palette.label
                font.family: Theme.font.sans
                font.pointSize: Theme.font.size.normal
                clip: true
                onTextChanged: root.query = text
                Keys.onEscapePressed: text = ""

                StyledText {
                    anchors.verticalCenter: parent.verticalCenter
                    visible: !search.text
                    text: "Search"
                    color: Theme.palette.tertiaryLabel
                }
            }
        }

        ListView {
            id: pageList

            acceptedButtons: Qt.NoButton  // the wheel scrolls; a mouse drag is for what it lands on

            anchors.top: searchBox.bottom
            anchors.topMargin: 12
            anchors.bottom: parent.bottom
            x: 8
            width: parent.width - 16
            clip: true
            spacing: 2
            model: SettingsPages.pages
            boundsBehavior: Flickable.StopAtBounds
            // The open page, kept in view (opened from elsewhere, it may be
            // below the fold).
            currentIndex: root.query === "" ? SettingsPages.pages.findIndex(p => p.name === root.page) : -1
            highlightFollowsCurrentItem: false
            onCurrentIndexChanged: if (currentIndex >= 0) positionViewAtIndex(currentIndex, ListView.Contain)
            Component.onCompleted: if (currentIndex >= 0) positionViewAtIndex(currentIndex, ListView.Contain)

            delegate: Rectangle {
                id: entry

                required property var modelData
                readonly property bool current: root.query === "" && root.page === modelData.name

                width: pageList.width
                height: 36
                radius: 9
                color: current ? Theme.palette.accentFill
                     : area.containsMouse ? Theme.palette.quaternaryFill : "transparent"

                Rectangle {
                    id: square

                    x: 8
                    anchors.verticalCenter: parent.verticalCenter
                    width: 24
                    height: 24
                    radius: 7
                    color: entry.modelData.color

                    MaterialIcon {
                        anchors.centerIn: parent
                        text: entry.modelData.icon
                        fill: 1
                        font.pointSize: Theme.font.size.small
                        color: Theme.dark.label
                    }
                }

                StyledText {
                    anchors.left: square.right
                    anchors.leftMargin: 10
                    anchors.right: parent.right
                    anchors.rightMargin: 8
                    anchors.verticalCenter: parent.verticalCenter
                    text: entry.modelData.name
                    elide: Text.ElideRight
                    font.weight: entry.current ? Font.DemiBold : Font.Normal
                }

                MouseArea {
                    id: area

                    anchors.fill: parent
                    hoverEnabled: true
                    onClicked: {
                        search.text = "";
                        root.page = entry.modelData.name;
                    }
                }
            }
        }
    }

    // --- the page ----------------------------------------------------------
    Flickable {
        id: body

        acceptedButtons: Qt.NoButton  // the wheel scrolls; a mouse drag is for what it lands on

        anchors.left: sidebar.right
        anchors.leftMargin: sidebar.inset
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        contentHeight: content.implicitHeight + 48
        clip: true
        boundsBehavior: Flickable.StopAtBounds

        Column {
            id: content

            x: 32
            y: 24
            width: body.width - 64
            spacing: 16

            StyledText {
                text: root.query ? "Search Results" : root.page
                font.pointSize: 20
                font.weight: Font.Bold
            }

            // Search: matching settings from every page, each with its page.
            Card {
                visible: root.query !== ""
                rows: SettingsPages.search(root.query)
                showPage: true
            }

            StyledText {
                visible: root.query !== "" && SettingsPages.search(root.query).length === 0
                text: "No settings match."
                color: Theme.palette.secondaryLabel
            }

            // Why a change didn't take, for a moment.
            Rectangle {
                visible: refusal.text.length > 0
                width: parent.width
                height: refusal.implicitHeight + 20
                radius: 10
                color: Theme.palette.tertiaryFill
                border.width: 1
                border.color: Theme.palette.red

                StyledText {
                    id: refusal

                    x: 14
                    anchors.verticalCenter: parent.verticalCenter
                    width: parent.width - 28
                    wrapMode: Text.WordWrap
                    color: Theme.palette.red
                }

                Timer {
                    id: clearRefusal

                    interval: 5000
                    onTriggered: refusal.text = ""
                }

                Connections {
                    target: Atrium

                    function onRefused(why: string): void {
                        refusal.text = why.charAt(0).toUpperCase() + why.slice(1) + ".";
                        clearRefusal.restart();
                    }
                }
            }

            AboutPage {
                visible: root.query === "" && root.page === "About"
                width: parent.width
            }

            AppsPage {
                visible: root.query === "" && root.page === "Apps"
                width: parent.width
            }

            NetworkPage {
                visible: root.query === "" && root.page === "Wi-Fi & Network"
                width: parent.width
            }

            BluetoothPage {
                visible: root.query === "" && root.page === "Bluetooth"
                width: parent.width
            }

            SoundPage {
                visible: root.query === "" && root.page === "Sound"
                width: parent.width
            }

            DisksPage {
                visible: root.query === "" && root.page === "Disks"
                width: parent.width
            }

            NotificationAppsPage {
                visible: root.query === "" && root.page === "Notifications"
                width: parent.width
            }

            PrintersPage {
                visible: root.query === "" && root.page === "Printers"
                width: parent.width
            }

            SoftwareUpdatePage {
                visible: root.query === "" && root.page === "Software Update"
                width: parent.width
            }

            PowerPage {
                visible: root.query === "" && root.page === "Power"
                width: parent.width
            }

            DefaultAppsPage {
                visible: root.query === "" && root.page === "Default Apps"
                width: parent.width
            }

            LoginItemsPage {
                visible: root.query === "" && root.page === "Login Items"
                width: parent.width
            }

            RegionPage {
                visible: root.query === "" && root.page === "Language & Region"
                width: parent.width
            }

            DateTimePage {
                visible: root.query === "" && root.page === "Date & Time"
                width: parent.width
            }

            KeyboardPage {
                visible: root.query === "" && root.page === "Keyboard"
                width: parent.width
            }

            DisplaysEditor {
                visible: root.query === "" && root.page === "Displays"
                width: parent.width
            }

            NightLightGroup {
                visible: root.query === "" && root.page === "Displays"
                width: parent.width
            }

            UsersPage {
                visible: root.query === "" && root.page === "Users & Groups"
                width: parent.width
            }

            // A generated page: plain rows in one card, editors in their own.
            Card {
                visible: root.query === "" && root.page !== "About" && rows.length > 0
                rows: (SettingsPages.byPage[root.page] ?? []).filter(s => !root.wide(s.type) && !s.custom)
            }

            Repeater {
                model: root.query === "" ? (SettingsPages.byPage[root.page] ?? []).filter(s => root.wide(s.type) && !s.custom) : []

                Card {
                    required property var modelData

                    rows: [modelData]
                }
            }

            // Under the shared settings: each device's own.
            DevicesEditor {
                visible: root.query === "" && root.page === "Mouse & Touchpad"
                width: parent.width
            }

            ShortcutsEditor {
                visible: root.query === "" && root.page === "Keyboard Shortcuts"
                width: parent.width
                window: root
            }

            RulesEditor {
                visible: root.query === "" && root.page === "Windows"
                width: parent.width
            }
        }
    }

    // The top of the page moves the window too (macOS's toolbar area), and
    // holds the traffic lights, on the right as atrium's title bars have them.
    MouseArea {
        anchors.left: sidebar.right
        anchors.right: parent.right
        anchors.top: parent.top
        height: 16
        onPressed: root.startMove()
        onDoubleClicked: root.toggleZoom()
    }

    TrafficLights {
        anchors.right: parent.right
        anchors.rightMargin: 16
        y: 16
        window: root
        onCloseRequested: root.visible = false
    }
}
