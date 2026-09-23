pragma ComponentBehavior: Bound

import QtQuick
import Quickshell
import qs.components
import qs.services
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
        return type === "keybinds" || type === "rules" || type === "list";
    }

    function openPage(name: string): void {
        if (name)
            page = name;
        query = "";
        search.text = "";
        visible = true;
    }

    title: "System Settings"
    visible: false
    color: Theme.palette.m3Surface
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

    // --- sidebar -----------------------------------------------------------
    Rectangle {
        id: sidebar

        width: 240
        height: parent.height
        color: Theme.palette.m3SurfaceContainer

        Rectangle {
            id: searchBox

            x: 12
            y: 14
            width: parent.width - 24
            height: 34
            radius: 10
            color: Theme.alpha(Theme.palette.m3OnSurface, 0.07)

            MaterialIcon {
                id: glass

                anchors.left: parent.left
                anchors.leftMargin: 10
                anchors.verticalCenter: parent.verticalCenter
                text: "search"
                font.pointSize: Theme.font.size.normal
                color: Theme.palette.m3OnSurfaceVariant
            }

            TextInput {
                id: search

                anchors.left: glass.right
                anchors.leftMargin: 6
                anchors.right: parent.right
                anchors.rightMargin: 10
                anchors.verticalCenter: parent.verticalCenter
                color: Theme.palette.m3OnSurface
                font.family: Theme.font.sans
                font.pointSize: Theme.font.size.normal
                clip: true
                onTextChanged: root.query = text
                Keys.onEscapePressed: text = ""

                StyledText {
                    anchors.verticalCenter: parent.verticalCenter
                    visible: !search.text
                    text: "Search"
                    color: Theme.alpha(Theme.palette.m3OnSurfaceVariant, 0.6)
                }
            }
        }

        ListView {
            id: pageList

            anchors.top: searchBox.bottom
            anchors.topMargin: 12
            anchors.bottom: parent.bottom
            x: 8
            width: parent.width - 16
            clip: true
            spacing: 2
            model: SettingsPages.pages
            boundsBehavior: Flickable.StopAtBounds

            delegate: Rectangle {
                id: entry

                required property var modelData
                readonly property bool current: root.query === "" && root.page === modelData.name

                width: pageList.width
                height: 36
                radius: 9
                color: current ? Theme.palette.m3PrimaryContainer
                     : area.containsMouse ? Theme.alpha(Theme.palette.m3OnSurface, 0.06) : "transparent"

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
                        color: "white"
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

        anchors.left: sidebar.right
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
                color: Theme.palette.m3OnSurfaceVariant
            }

            AboutPage {
                visible: root.query === "" && root.page === "About"
                width: parent.width
            }

            // A generated page: plain rows in one card, editors in their own.
            Card {
                visible: root.query === "" && root.page !== "About" && rows.length > 0
                rows: (SettingsPages.byPage[root.page] ?? []).filter(s => !root.wide(s.type))
            }

            Repeater {
                model: root.query === "" ? (SettingsPages.byPage[root.page] ?? []).filter(s => root.wide(s.type)) : []

                Card {
                    required property var modelData

                    rows: [modelData]
                }
            }
        }
    }
}
