pragma ComponentBehavior: Bound

import QtQuick
import Atrium
import Atrium.Shell
import shell.components
import shell.services

// The apps that open at login, as macOS's Login Items: the user's own and
// the system's (which can be turned off, not removed), and more to add.
Column {
    id: root

    spacing: 20

    Group {
        title: "Open at Login"
        subtitle: Autostart.entries.length === 0 ? "Nothing opens at login yet." : ""
        headerActions: [
            PillButton {
                text: "Add…"
                icon: "add"
                onClicked: addSheet.open()
            }
        ]

        Repeater {
            model: Autostart.entries

            Item {
                id: item

                required property var modelData

                width: parent.width
                height: 48

                IconImage {
                    x: 8
                    anchors.verticalCenter: parent.verticalCenter
                    implicitSize: 28
                    source: Shell.iconPath(item.modelData.icon || "application-x-executable")
                }

                StyledText {
                    x: 48
                    width: parent.width - x - controls.width - 16
                    anchors.verticalCenter: parent.verticalCenter
                    text: item.modelData.name
                    elide: Text.ElideRight
                    opacity: item.modelData.enabled ? 1 : 0.55
                }

                Row {
                    id: controls

                    anchors.right: parent.right
                    anchors.rightMargin: 8
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 10

                    PillButton {
                        anchors.verticalCenter: parent.verticalCenter
                        visible: item.modelData.own
                        icon: "remove"
                        onClicked: Autostart.remove(item.modelData.id)
                    }

                    Switch {
                        anchors.verticalCenter: parent.verticalCenter
                        checked: item.modelData.enabled
                        onToggled: Autostart.setEnabled(item.modelData.id, !checked)
                    }
                }
            }
        }
    }

    Sheet {
        id: addSheet

        property string picked: ""

        title: "Open at Login"
        action: "Add"
        ready: picked !== ""
        onOpened: {
            picked = "";
            search.text = "";
            search.focusField();
        }
        onSubmitted: {
            Autostart.add(picked);
            close();
        }

        Field {
            id: search

            width: parent.width
            placeholder: "Search apps"
        }

        ListView {
            id: apps

            width: parent.width
            height: 280
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            model: addSheet.visible ? Autostart.candidates(search.text) : []

            delegate: Rectangle {
                id: app

                required property var modelData
                readonly property bool chosen: addSheet.picked === modelData.id

                width: apps.width
                height: 34
                radius: 7
                color: chosen ? Theme.palette.accent
                     : hover.containsMouse ? Theme.palette.tertiaryFill : "transparent"

                IconImage {
                    x: 8
                    anchors.verticalCenter: parent.verticalCenter
                    implicitSize: 22
                    source: Shell.iconPath(app.modelData.icon || "application-x-executable")
                }

                StyledText {
                    x: 38
                    width: parent.width - 46
                    anchors.verticalCenter: parent.verticalCenter
                    text: app.modelData.name
                    elide: Text.ElideRight
                    font.pointSize: Theme.font.size.small
                    color: app.chosen ? Theme.palette.labelOnAccent : Theme.palette.label
                }

                MouseArea {
                    id: hover

                    anchors.fill: parent
                    hoverEnabled: true
                    onClicked: addSheet.picked = app.modelData.id
                    onDoubleClicked: {
                        addSheet.picked = app.modelData.id;
                        addSheet.submitted();
                    }
                }
            }
        }
    }
}
