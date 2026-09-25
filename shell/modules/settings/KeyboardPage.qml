pragma ComponentBehavior: Bound

import QtQuick
import Atrium
import shell.components
import shell.services

// The keyboard layouts in use, as macOS's Input Sources: the list (the
// first is where new windows start), one to add from xkeyboard-config's
// list, and the rest of the keyboard's settings under it.
Column {
    id: root

    readonly property var sources: KeyboardLayouts.sources
    readonly property int active: Atrium.keyboard.active ?? 0

    spacing: 20

    Group {
        title: "Input Sources"
        subtitle: root.sources.length > 1 ? "Switch between them from the menu bar." : ""
        headerActions: [
            PillButton {
                text: "Add…"
                icon: "add"
                onClicked: addSheet.open()
            }
        ]

        Repeater {
            model: root.sources

            DeviceRow {
                id: row

                required property var modelData
                required property int index

                glyph: "keyboard"
                name: modelData.label
                note: root.sources.length > 1 && index === root.active ? "In use" : ""
                active: root.sources.length > 1 && index === root.active
                onClicked: Atrium.setKeyboardLayout(index)

                PillButton {
                    visible: row.index > 0
                    icon: "arrow_upward"
                    onClicked: KeyboardLayouts.moveUp(row.index)
                }

                PillButton {
                    visible: root.sources.length > 1
                    icon: "remove"
                    onClicked: KeyboardLayouts.remove(row.index)
                }
            }
        }
    }

    Sheet {
        id: addSheet

        property var picked: null

        title: "Add an Input Source"
        action: "Add"
        ready: picked !== null
        onOpened: {
            picked = null;
            search.text = "";
            search.focusField();
        }
        onSubmitted: {
            KeyboardLayouts.add(picked.layout, picked.variant);
            close();
        }

        Field {
            id: search

            width: parent.width
            placeholder: "Search"
        }

        ListView {
            id: results

            acceptedButtons: Qt.NoButton

            width: parent.width
            height: 280
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            model: KeyboardLayouts.find(search.text)

            delegate: Rectangle {
                id: result

                required property var modelData
                readonly property bool chosen: addSheet.picked?.layout === modelData.layout
                                               && addSheet.picked?.variant === modelData.variant

                width: results.width
                height: 30
                radius: 7
                color: chosen ? Theme.palette.accent
                     : hover.containsMouse ? Theme.palette.tertiaryFill : "transparent"

                StyledText {
                    x: 10
                    width: parent.width - 20
                    anchors.verticalCenter: parent.verticalCenter
                    text: result.modelData.label
                    elide: Text.ElideRight
                    font.pointSize: Theme.font.size.small
                    color: result.chosen ? Theme.palette.labelOnAccent : Theme.palette.label
                }

                MouseArea {
                    id: hover

                    anchors.fill: parent
                    hoverEnabled: true
                    onClicked: addSheet.picked = result.modelData
                    onDoubleClicked: {
                        addSheet.picked = result.modelData;
                        addSheet.submitted();
                    }
                }
            }
        }
    }
}
