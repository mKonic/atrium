pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Effects
import Atrium.Shell
import shell.components
import shell.services
import Atrium

// Super+Period: every emoji, by group and searchable by name. Picking one
// types it where you were typing; with no text field there, it is copied.
PanelWindow {
    id: picker

    property string query: ""
    readonly property var results: Emojis.find(query)
    readonly property var hovered: grid.currentItem ? results[grid.currentIndex] : null
    // Material icons for Unicode's groups, in its order.
    readonly property var groupIcons: ({
            "Smileys & Emotion": "sentiment_satisfied",
            "People & Body": "waving_hand",
            "Animals & Nature": "pets",
            "Food & Drink": "restaurant",
            "Travel & Places": "directions_car",
            "Activities": "sports_soccer",
            "Objects": "lightbulb",
            "Symbols": "emoji_symbols",
            "Flags": "flag"
        })

    visible: false
    screen: Shell.screen(Atrium.focusedOutput?.name)
    anchors {
        top: true
        bottom: true
        left: true
        right: true
    }
    exclusiveZone: -1
    color: "transparent"
    WlrLayershell.layer: WlrLayer.Overlay
    WlrLayershell.keyboardFocus: visible ? WlrKeyboardFocus.Exclusive : WlrKeyboardFocus.None
    WlrLayershell.namespace: "atrium-emoji"

    function open(): void {
        search.text = "";
        grid.currentIndex = 0;
        grid.positionViewAtBeginning();
        visible = true;
        search.forceActiveFocus();
        shown.restart();
    }

    function close(): void {
        visible = false;
    }

    function pick(text: string): void {
        // Away first, so the field it goes into has the keyboard back.
        close();
        Emojis.pick(text);
    }

    Connections {
        target: Atrium

        function onShellAction(name: string): void {
            if (name === "emoji")
                picker.visible ? picker.close() : picker.open();
        }

        function onTextNotInserted(text: string): void {
            Shell.clipboardText = text;
        }
    }

    MouseArea {
        anchors.fill: parent
        onClicked: picker.close()
    }

    Rectangle {
        id: panel

        anchors.horizontalCenter: parent.horizontalCenter
        y: Math.round(picker.height * 0.16)
        width: Math.min(560, picker.width - 64)
        height: Math.min(520, picker.height - y - 48)
        radius: 26
        color: Theme.material.regular

        Glass {}

        border.width: Theme.lens ? 0 : 1
        border.color: Theme.palette.separator

        layer.enabled: true
        layer.effect: MultiEffect {
            shadowEnabled: !Theme.lens
            shadowColor: Theme.palette.shadow
            shadowBlur: 1
            shadowVerticalOffset: 8
        }

        ParallelAnimation {
            id: shown

            Anim {
                target: panel
                property: "scale"
                from: 0.96
                to: 1
                duration: Theme.anim.small
                easing.bezierCurve: Theme.anim.emphasizedDecel
            }
            Anim {
                target: panel
                property: "opacity"
                from: 0
                to: 1
                duration: Theme.anim.small
            }
        }

        MouseArea {
            anchors.fill: parent  // clicks on the panel stay on it
        }

        Item {
            id: header

            width: parent.width
            height: 56

            MaterialIcon {
                id: glyph

                anchors.left: parent.left
                anchors.leftMargin: 20
                anchors.verticalCenter: parent.verticalCenter
                text: "search"
                font.pointSize: 16
                color: Theme.palette.secondaryLabel
            }

            TextInput {
                id: search

                anchors.left: glyph.right
                anchors.leftMargin: 12
                anchors.right: parent.right
                anchors.rightMargin: 20
                anchors.verticalCenter: parent.verticalCenter
                color: Theme.palette.label
                font.family: Theme.font.sans
                font.pointSize: 15
                selectionColor: Theme.palette.accent
                selectedTextColor: Theme.palette.labelOnAccent
                clip: true
                onTextChanged: {
                    picker.query = text;
                    grid.currentIndex = 0;
                }

                Keys.onPressed: event => {
                    if (event.key === Qt.Key_Escape)
                        picker.close();
                    else if (event.key === Qt.Key_Right)
                        grid.moveCurrentIndexRight();
                    else if (event.key === Qt.Key_Left && search.text.length === 0)
                        grid.moveCurrentIndexLeft();
                    else if (event.key === Qt.Key_Down)
                        grid.moveCurrentIndexDown();
                    else if (event.key === Qt.Key_Up)
                        grid.moveCurrentIndexUp();
                    else if ((event.key === Qt.Key_Return || event.key === Qt.Key_Enter) && picker.hovered)
                        picker.pick(picker.hovered.text);
                    else
                        return;
                    event.accepted = true;
                }

                StyledText {
                    anchors.fill: parent
                    visible: search.text.length === 0
                    text: "Search emoji"
                    font.pointSize: 15
                    color: Theme.palette.tertiaryLabel
                }
            }
        }

        // One tab per group: jumps there (while nothing is searched for).
        Row {
            id: tabs

            anchors.top: header.bottom
            anchors.horizontalCenter: parent.horizontalCenter
            spacing: 4
            visible: picker.query.length === 0

            Repeater {
                model: Emojis.groups

                Rectangle {
                    id: tab

                    required property string modelData
                    readonly property bool here: picker.hovered?.group === modelData

                    width: 40
                    height: 32
                    radius: 10
                    color: here ? Theme.palette.accentFill
                         : tabArea.containsMouse ? Theme.palette.tertiaryFill : "transparent"

                    MaterialIcon {
                        anchors.centerIn: parent
                        text: picker.groupIcons[tab.modelData] ?? "category"
                        color: tab.here ? Theme.palette.accent : Theme.palette.secondaryLabel
                    }

                    MouseArea {
                        id: tabArea

                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            const i = Emojis.firstOf(tab.modelData);
                            grid.currentIndex = i;
                            grid.positionViewAtIndex(i, GridView.Beginning);
                        }
                    }
                }
            }
        }

        GridView {
            id: grid

            anchors.top: tabs.visible ? tabs.bottom : header.bottom
            anchors.topMargin: 8
            anchors.bottom: footer.top
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.leftMargin: 16
            anchors.rightMargin: 16
            clip: true
            cellWidth: Math.floor(width / Math.floor(width / 46))
            cellHeight: 46
            model: picker.results
            highlightMoveDuration: 0
            highlight: Rectangle {
                radius: 10
                color: Theme.palette.accentFill
            }

            delegate: Item {
                id: cell

                required property var modelData
                required property int index

                width: grid.cellWidth
                height: grid.cellHeight

                Text {
                    anchors.centerIn: parent
                    text: cell.modelData.text
                    font.pixelSize: 28
                    font.family: Emojis.font
                }

                MouseArea {
                    anchors.fill: parent
                    hoverEnabled: true
                    onEntered: grid.currentIndex = cell.index
                    onClicked: picker.pick(cell.modelData.text)
                }
            }

            StyledText {
                anchors.centerIn: parent
                visible: picker.results.length === 0
                text: "No emoji match"
                color: Theme.palette.secondaryLabel
            }
        }

        // What the one under the pointer (or the arrow keys) is called.
        Item {
            id: footer

            anchors.bottom: parent.bottom
            width: parent.width
            height: 44

            Text {
                id: big

                anchors.left: parent.left
                anchors.leftMargin: 20
                anchors.verticalCenter: parent.verticalCenter
                text: picker.hovered?.text ?? ""
                font.pixelSize: 22
                font.family: Emojis.font
            }

            StyledText {
                anchors.left: big.right
                anchors.leftMargin: 10
                anchors.right: parent.right
                anchors.rightMargin: 20
                anchors.verticalCenter: parent.verticalCenter
                text: picker.hovered?.name ?? ""
                elide: Text.ElideRight
                color: Theme.palette.secondaryLabel
            }
        }
    }
}
