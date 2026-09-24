pragma ComponentBehavior: Bound

import QtQuick
import Atrium
import Atrium.Shell
import shell.components
import shell.services

// Each app that has sent notifications, and what they do: pop up, go
// quietly to the notification center, or nothing at all.
Column {
    spacing: 20

    SectionHeader {
        visible: NotificationHistory.apps.length === 0
        width: parent.width
        title: "Apps"
        subtitle: "Apps show up here once they've sent a notification."
    }

    Group {
        visible: NotificationHistory.apps.length > 0
        title: "Apps"
        subtitle: "Quiet ones go straight to the notification center."

        Repeater {
            model: NotificationHistory.apps

            Item {
                id: app

                required property var modelData

                width: parent.width
                height: 48

                IconImage {
                    x: 8
                    anchors.verticalCenter: parent.verticalCenter
                    implicitSize: 26
                    source: app.modelData.icon || Shell.iconPath("preferences-desktop-notification")
                }

                StyledText {
                    x: 46
                    width: parent.width - x - mode.width - 16
                    anchors.verticalCenter: parent.verticalCenter
                    text: app.modelData.name
                    elide: Text.ElideRight
                }

                ChoiceControl {
                    id: mode

                    anchors.right: parent.right
                    anchors.rightMargin: 8
                    anchors.verticalCenter: parent.verticalCenter
                    value: app.modelData.mode
                    choices: ["on", "quiet", "off"]
                    onPicked: v => NotificationHistory.setAppMode(app.modelData.name, v)
                }
            }
        }
    }
}
