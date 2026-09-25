pragma ComponentBehavior: Bound

import QtQuick
import Atrium
import shell.components
import shell.services

// The clock: network time and the time zone, set for the whole computer
// through timedated (asking for a password when the system wants one).
Column {
    id: root

    property string error: ""  // why the last change didn't happen

    spacing: 20

    Connections {
        target: DateTime

        function onFailed(why: string): void {
            root.error = why;
        }
    }

    Group {
        subtitle: root.error

        ControlRow {
            title: "Set time automatically"
            note: DateTime.canNtp ? "From the internet (systemd-timesyncd)." : "No network time service is installed."

            Switch {
                enabled: DateTime.canNtp
                opacity: enabled ? 1 : 0.4
                checked: DateTime.ntp
                onToggled: {
                    root.error = "";
                    DateTime.setNtp(!checked);
                }
            }
        }

        ControlRow {
            title: "Time zone"
            note: DateTime.timezoneLabel

            PillButton {
                text: "Change…"
                onClicked: zoneSheet.open()
            }
        }
    }

    Sheet {
        id: zoneSheet

        property string picked: ""

        title: "Time Zone"
        action: "Set"
        ready: picked !== "" && picked !== DateTime.timezone
        onOpened: {
            root.error = "";
            picked = "";
            search.text = "";
            search.focusField();
        }
        onSubmitted: {
            DateTime.setTimezone(picked);
            close();
        }

        Field {
            id: search

            width: parent.width
            placeholder: "Search for a city"
        }

        ListView {
            id: zones

            acceptedButtons: Qt.NoButton

            width: parent.width
            height: 280
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            model: zoneSheet.visible ? DateTime.findTimezones(search.text) : []

            delegate: Rectangle {
                id: zone

                required property var modelData
                readonly property bool chosen: zoneSheet.picked === modelData.value

                width: zones.width
                height: 30
                radius: 7
                color: chosen ? Theme.palette.accent
                     : hover.containsMouse ? Theme.palette.tertiaryFill : "transparent"

                StyledText {
                    x: 10
                    width: parent.width - 20
                    anchors.verticalCenter: parent.verticalCenter
                    text: zone.modelData.label
                    elide: Text.ElideRight
                    font.pointSize: Theme.font.size.small
                    color: zone.chosen ? Theme.palette.labelOnAccent : Theme.palette.label
                }

                MouseArea {
                    id: hover

                    anchors.fill: parent
                    hoverEnabled: true
                    onClicked: zoneSheet.picked = zone.modelData.value
                    onDoubleClicked: {
                        zoneSheet.picked = zone.modelData.value;
                        zoneSheet.submitted();
                    }
                }
            }
        }
    }
}
