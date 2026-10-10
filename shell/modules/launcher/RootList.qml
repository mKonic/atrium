pragma ComponentBehavior: Bound

import QtQuick
import Atrium.Shell
import shell.components
import shell.services
import Atrium

// The root search and the one-kind screens (Switch Windows, Search Snippets,
// Search Quicklinks): LauncherModel's rows, under their section headers.
ListView {
    id: list

    required property LauncherModel launcherModel
    // Ctrl held a moment: favorites show their Ctrl+N instead of what they are.
    property bool ctrlHeld: false
    signal activated(int row)

    function handleKey(event: var): bool {
        const ctrl = event.modifiers & Qt.ControlModifier;
        const shift = event.modifiers & Qt.ShiftModifier;
        const alt = event.modifiers & Qt.AltModifier;
        if (event.key === Qt.Key_Down || (ctrl && event.key === Qt.Key_N)) {
            launcherModel.move(1);
        } else if (event.key === Qt.Key_Up || (ctrl && event.key === Qt.Key_P)) {
            launcherModel.move(-1);
        } else if (event.key === Qt.Key_PageDown) {
            launcherModel.current = Math.min(launcherModel.count - 1, launcherModel.current + 8);
        } else if (event.key === Qt.Key_PageUp) {
            launcherModel.current = Math.max(0, launcherModel.current - 8);
        } else if (ctrl && !shift && event.key >= Qt.Key_0 && event.key <= Qt.Key_9) {
            // The number row: Ctrl+1..9, Ctrl+0 for the tenth.
            return launcherModel.openFavorite(event.key === Qt.Key_0 ? 10 : event.key - Qt.Key_0);
        } else if (ctrl && shift && event.key === Qt.Key_F) {
            return launcherModel.chord("favorite");
        } else if (ctrl && shift && event.key === Qt.Key_H) {
            return launcherModel.chord("hide");
        } else if (ctrl && shift && event.key === Qt.Key_Q) {
            return launcherModel.chord("quit");
        } else if (ctrl && shift && event.key === Qt.Key_C) {
            return launcherModel.chord("copy");
        } else if (ctrl && !shift && event.key === Qt.Key_R) {
            return launcherModel.chord("restart");
        } else if (ctrl && alt && event.key === Qt.Key_Up) {
            return launcherModel.chord("favorite-up");
        } else if (ctrl && alt && event.key === Qt.Key_Down) {
            return launcherModel.chord("favorite-down");
        } else if (ctrl && (event.key === Qt.Key_Return || event.key === Qt.Key_Enter)) {
            return launcherModel.chord("reveal") || launcherModel.chord("type");
        } else {
            return false;
        }
        return true;
    }

    model: launcherModel
    clip: true
    currentIndex: launcherModel.current
    boundsBehavior: Flickable.StopAtBounds
    highlightMoveDuration: 0
    highlightFollowsCurrentItem: false
    reuseItems: true
    topMargin: 6
    bottomMargin: 6
    onCurrentIndexChanged: positionViewAtIndex(currentIndex, ListView.Contain)

    section.property: "section"
    section.criteria: ViewSection.FullString
    section.delegate: Item {
        id: header

        required property string section

        width: list.width
        height: section.length > 0 ? 30 : 0

        StyledText {
            anchors.left: parent.left
            anchors.leftMargin: 20
            anchors.bottom: parent.bottom
            anchors.bottomMargin: 6
            visible: header.section.length > 0
            text: header.section
            font.pointSize: Theme.font.size.small
            font.weight: Font.Medium
            color: Theme.palette.secondaryLabel
        }
    }

    delegate: Item {
        id: row

        required property int index
        required property string kind
        required property string title
        required property string detail
        required property string icon
        required property string glyph
        required property string color
        required property string label
        required property string alias
        required property string keys
        required property int slot
        required property bool running
        required property string badge
        readonly property bool selected: index === list.launcherModel.current
        readonly property bool card: kind === "calc"

        // For screen readers (AT-SPI): a result, read as the selection moves.
        Accessible.role: Accessible.ListItem
        Accessible.name: title
        Accessible.description: detail
        Accessible.selected: selected
        Accessible.onPressAction: list.activated(index)

        width: list.width
        height: card ? 92 : 44

        Rectangle {
            anchors.fill: parent
            anchors.leftMargin: 8
            anchors.rightMargin: 8
            radius: 10
            color: row.selected ? Theme.palette.accentFill : "transparent"
        }

        // A calculation: the question and what it is, the answer and what it is.
        Row {
            anchors.centerIn: parent
            visible: row.card
            spacing: 0

            component Side: Column {
                property string text
                property string badge
                property bool answer

                width: (list.width - 90) / 2
                spacing: 6

                StyledText {
                    anchors.horizontalCenter: parent.horizontalCenter
                    width: Math.min(implicitWidth, parent.width)
                    text: parent.text
                    elide: Text.ElideMiddle
                    font.pointSize: parent.answer ? Theme.font.size.large + 2 : Theme.font.size.large
                    font.weight: parent.answer ? Font.DemiBold : Font.Normal
                    color: parent.answer ? Theme.palette.label : Theme.palette.secondaryLabel
                }

                Rectangle {
                    anchors.horizontalCenter: parent.horizontalCenter
                    visible: parent.badge.length > 0
                    width: badgeText.implicitWidth + 14
                    height: 20
                    radius: 10
                    color: Theme.palette.tertiaryFill

                    StyledText {
                        id: badgeText

                        anchors.centerIn: parent
                        text: parent.parent.badge
                        font.pointSize: Theme.font.size.small
                        color: Theme.palette.secondaryLabel
                    }
                }
            }

            Side {
                text: row.detail
                badge: row.badge
            }

            MaterialIcon {
                anchors.verticalCenter: parent.verticalCenter
                width: 58
                horizontalAlignment: Text.AlignHCenter
                text: "arrow_forward"
                font.pointSize: 16
                color: Theme.palette.tertiaryLabel
            }

            Side {
                text: row.title
                badge: row.label
                answer: true
            }
        }

        // The app's own icon, else a glyph (on its colour, for Settings pages).
        Item {
            id: art

            visible: !row.card
            anchors.left: parent.left
            anchors.leftMargin: 20
            anchors.verticalCenter: parent.verticalCenter
            width: 26
            height: 26

            IconImage {
                anchors.fill: parent
                visible: row.icon.length > 0
                implicitSize: 26
                source: row.icon.length === 0 ? "" : row.icon.startsWith("file:") ? row.icon : Shell.iconPath(row.icon, "application-x-executable")
                asynchronous: true
            }

            Rectangle {
                anchors.fill: parent
                visible: row.icon.length === 0
                radius: 7
                color: row.color.length > 0 ? row.color : Theme.palette.tertiaryFill

                MaterialIcon {
                    anchors.centerIn: parent
                    text: row.glyph
                    font.pointSize: 13
                    color: row.color.length > 0 ? "white" : Theme.palette.label
                }
            }

            // Running: a dot under it, as in the Dock.
            Rectangle {
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.top: parent.bottom
                anchors.topMargin: 1
                visible: row.running
                width: 4
                height: 4
                radius: 2
                color: Theme.palette.secondaryLabel
            }
        }

        Row {
            id: words

            visible: !row.card
            anchors.left: art.right
            anchors.leftMargin: 12
            anchors.right: trailing.left
            anchors.rightMargin: 12
            anchors.verticalCenter: parent.verticalCenter
            spacing: 8

            StyledText {
                id: titleText

                anchors.verticalCenter: parent.verticalCenter
                width: Math.min(implicitWidth, words.width - (aliasChip.visible ? aliasChip.width + 8 : 0))
                text: row.title
                elide: Text.ElideRight
                font.pointSize: row.kind === "calc" ? Theme.font.size.large : Theme.font.size.normal
                font.weight: row.kind === "calc" ? Font.Medium : Font.Normal
            }

            Rectangle {
                id: aliasChip

                anchors.verticalCenter: parent.verticalCenter
                visible: row.alias.length > 0
                width: aliasText.implicitWidth + 12
                height: 18
                radius: 5
                border.width: 1
                border.color: Theme.palette.separator
                color: "transparent"

                StyledText {
                    id: aliasText

                    anchors.centerIn: parent
                    text: row.alias
                    font.pointSize: Theme.font.size.small
                    color: Theme.palette.secondaryLabel
                }
            }

            StyledText {
                anchors.verticalCenter: parent.verticalCenter
                width: Math.max(0, words.width - titleText.width - (aliasChip.visible ? aliasChip.width + 8 : 0) - 8)
                visible: row.detail.length > 0 && width > 30
                text: row.detail
                elide: Text.ElideRight
                font.pointSize: Theme.font.size.smaller
                color: Theme.palette.tertiaryLabel
            }
        }

        // What it is, its global shortcut, or (Ctrl held) its favorite number.
        Item {
            id: trailing

            visible: !row.card
            anchors.right: parent.right
            anchors.rightMargin: 20
            anchors.verticalCenter: parent.verticalCenter
            width: Math.max(kindText.visible ? kindText.implicitWidth : 0, caps.visible ? caps.implicitWidth : 0)
            height: 20

            Keycap {
                id: caps

                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                visible: (list.ctrlHeld && row.slot > 0) || (!list.ctrlHeld && row.keys.length > 0)
                keys: list.ctrlHeld && row.slot > 0 ? `Ctrl+${row.slot % 10}` : row.keys
                dim: true
            }

            StyledText {
                id: kindText

                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                visible: !caps.visible
                text: row.label
                font.pointSize: Theme.font.size.smaller
                color: Theme.palette.tertiaryLabel
            }
        }

        MouseArea {
            anchors.fill: parent
            hoverEnabled: true
            // Only real pointer movement picks a row, not a list scrolling under a still pointer.
            onPositionChanged: list.launcherModel.current = row.index
            onClicked: list.activated(row.index)
        }
    }

    Column {
        anchors.centerIn: parent
        visible: list.count === 0
        spacing: 8

        MaterialIcon {
            anchors.horizontalCenter: parent.horizontalCenter
            text: "search_off"
            font.pointSize: 26
            color: Theme.palette.tertiaryLabel
        }

        StyledText {
            anchors.horizontalCenter: parent.horizontalCenter
            text: "No Results"
            color: Theme.palette.secondaryLabel
        }
    }
}
