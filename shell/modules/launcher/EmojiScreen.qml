pragma ComponentBehavior: Bound

import QtQuick
import shell.components
import shell.services
import Atrium

// Search Emoji & Symbols: every emoji by group, filtered by the palette's
// field. Enter types it where you were typing (copied when nothing takes it).
Item {
    id: screen

    required property LauncherModel launcherModel
    readonly property var results: Emojis.find(launcherModel.query)
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
    readonly property var hints: [["Type", "Enter"]]
    signal done

    function pick(text: string): void {
        done();
        Emojis.pick(text);
    }

    function handleKey(event: var): bool {
        const ctrl = event.modifiers & Qt.ControlModifier;
        if (event.key === Qt.Key_Right || (ctrl && event.key === Qt.Key_F))
            grid.moveCurrentIndexRight();
        else if ((event.key === Qt.Key_Left && launcherModel.query.length === 0) || (ctrl && event.key === Qt.Key_B))
            grid.moveCurrentIndexLeft();
        else if (event.key === Qt.Key_Down || (ctrl && event.key === Qt.Key_N))
            grid.moveCurrentIndexDown();
        else if (event.key === Qt.Key_Up || (ctrl && event.key === Qt.Key_P))
            grid.moveCurrentIndexUp();
        else if ((event.key === Qt.Key_Return || event.key === Qt.Key_Enter) && hovered)
            pick(hovered.text);
        else
            return false;
        return true;
    }

    onResultsChanged: grid.currentIndex = 0

    Row {
        id: tabs

        anchors.top: parent.top
        anchors.topMargin: 6
        anchors.horizontalCenter: parent.horizontalCenter
        spacing: 4
        visible: screen.launcherModel.query.length === 0
        height: visible ? 32 : 0

        Repeater {
            model: Emojis.groups

            Rectangle {
                id: tab

                required property string modelData
                readonly property bool here: screen.hovered?.group === modelData

                width: 40
                height: 32
                radius: 10
                color: here ? Theme.palette.accentFill : tabArea.containsMouse ? Theme.palette.tertiaryFill : "transparent"

                MaterialIcon {
                    anchors.centerIn: parent
                    text: screen.groupIcons[tab.modelData] ?? "category"
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

        anchors.top: tabs.bottom
        anchors.topMargin: 6
        anchors.bottom: parent.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.leftMargin: 14
        anchors.rightMargin: 14
        clip: true
        cellWidth: Math.floor(width / Math.floor(width / 48))
        cellHeight: 48
        model: screen.results
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
                onPositionChanged: grid.currentIndex = cell.index
                onClicked: screen.pick(cell.modelData.text)
            }
        }

        StyledText {
            anchors.centerIn: parent
            visible: screen.results.length === 0
            text: qsTr("No Results")
            color: Theme.palette.secondaryLabel
        }
    }
}
