pragma ComponentBehavior: Bound

import QtQuick
import shell.components
import shell.services
import Atrium

// Rules for what an app's own record can't say: windows matched by title,
// or by a pattern over app ids.
Rectangle {
    id: root

    width: parent?.width ?? 0
    height: column.implicitHeight + 32
    radius: 14
    color: Theme.palette.m3SurfaceContainer
    border.width: 1
    border.color: Theme.alpha(Theme.palette.m3Outline, 0.12)

    Column {
        id: column

        x: 16
        y: 16
        width: parent.width - 32
        spacing: 4

        SectionHeader {
            width: parent.width
            title: "Rules"
            subtitle: "Windows matched by title or an app id pattern. For a whole app, set where it opens under Apps."

            PillButton {
                text: "Add"
                icon: "add"
                primary: true
                onClicked: Atrium.addRule({ title_pattern: "Picture-in-Picture" })
            }
        }

        Item {
            width: 1
            height: 8
        }

        StyledText {
            visible: Atrium.rules.length === 0
            topPadding: 6
            bottomPadding: 6
            text: "No rules."
            color: Theme.palette.m3OnSurfaceVariant
        }

        Repeater {
            model: Atrium.rules

            Column {
                id: rule

                required property var modelData
                required property int index

                width: column.width
                spacing: 8
                topPadding: index > 0 ? 10 : 0
                bottomPadding: 10

                Rectangle {
                    visible: rule.index > 0
                    width: parent.width
                    height: 1
                    color: Theme.alpha(Theme.palette.m3Outline, 0.12)
                }

                Row {
                    spacing: 8

                    StyledText {
                        anchors.verticalCenter: parent.verticalCenter
                        width: 90
                        text: "App id"
                        font.pointSize: Theme.font.size.small
                        color: Theme.palette.m3OnSurfaceVariant
                    }

                    TextControl {
                        fieldWidth: 200
                        placeholder: "any"
                        value: rule.modelData.app_pattern
                        onCommitted: v => Atrium.setRule(rule.modelData.id, { app_pattern: v })
                    }

                    StyledText {
                        anchors.verticalCenter: parent.verticalCenter
                        text: "Title"
                        font.pointSize: Theme.font.size.small
                        color: Theme.palette.m3OnSurfaceVariant
                    }

                    TextControl {
                        fieldWidth: 200
                        placeholder: "any"
                        value: rule.modelData.title_pattern
                        onCommitted: v => Atrium.setRule(rule.modelData.id, { title_pattern: v })
                    }
                }

                Item {
                    width: parent.width
                    height: 30

                    StyledText {
                        anchors.verticalCenter: parent.verticalCenter
                        width: 90
                        text: "Opens in"
                        font.pointSize: Theme.font.size.small
                        color: Theme.palette.m3OnSurfaceVariant
                    }

                    Destination {
                        x: 98
                        anchors.verticalCenter: parent.verticalCenter
                        space: rule.modelData.space
                        secret: rule.modelData.secret
                        onChanged: fields => Atrium.setRule(rule.modelData.id, fields)
                    }

                    PillButton {
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        text: "Remove"
                        onClicked: Atrium.removeRule(rule.modelData.id)
                    }
                }
            }
        }
    }
}
