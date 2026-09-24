pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Effects
import Atrium.Shell
import shell.components
import shell.modules.settings
import shell.services
import Atrium

// "Allow Apps to Set Backgrounds?": the app's icon, the question, what it
// means, any choices, then Don't Allow and Allow.
Rectangle {
    id: root

    width: 380
    height: column.implicitHeight + 48
    radius: 26
    color: Theme.material.thick
    border.width: Theme.lens ? 0 : 1
    border.color: Theme.palette.separator
    focus: true
    Keys.onEscapePressed: AccessPrompt.answer(false)

    layer.enabled: true
    layer.effect: MultiEffect {
        shadowEnabled: !Theme.lens
        shadowColor: Theme.palette.shadow
        shadowBlur: 1
        shadowVerticalOffset: 8
    }

    Column {
        id: column

        anchors.horizontalCenter: parent.horizontalCenter
        y: 24
        width: parent.width - 48
        spacing: 10

        // The asking app's icon, else the portal's, else a shield.
        Item {
            anchors.horizontalCenter: parent.horizontalCenter
            width: 64
            height: 64

            IconImage {
                id: appIcon

                anchors.fill: parent
                visible: status === Image.Ready
                source: AccessPrompt.app ? Icons.appIcon(AccessPrompt.app)
                      : AccessPrompt.icon ? Shell.iconPath(AccessPrompt.icon, true) : ""
            }

            Rectangle {
                anchors.fill: parent
                visible: !appIcon.visible
                radius: 32
                color: Theme.palette.accent

                MaterialIcon {
                    anchors.centerIn: parent
                    text: "shield"
                    fill: 1
                    font.pointSize: 24
                    color: Theme.palette.labelOnAccent
                }
            }
        }

        StyledText {
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
            text: AccessPrompt.title
            font.weight: Font.DemiBold
        }

        StyledText {
            width: parent.width
            visible: text !== ""
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
            text: AccessPrompt.subtitle
            font.pointSize: Theme.font.size.small
            color: Theme.palette.secondaryLabel
        }

        StyledText {
            width: parent.width
            visible: text !== ""
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
            text: AccessPrompt.body
            font.pointSize: Theme.font.size.small
            color: Theme.palette.secondaryLabel
        }

        // Choices: a switch for yes-or-no ones, else a menu.
        Repeater {
            model: AccessPrompt.choices

            Item {
                id: choice

                required property var modelData
                readonly property bool toggle: modelData.options.length === 0

                width: column.width
                height: Math.max(36, label.implicitHeight)

                StyledText {
                    id: label

                    anchors.left: parent.left
                    anchors.right: control.left
                    anchors.rightMargin: 12
                    anchors.verticalCenter: parent.verticalCenter
                    wrapMode: Text.WordWrap
                    text: choice.modelData.label
                    font.pointSize: Theme.font.size.small
                }

                Item {
                    id: control

                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    width: childrenRect.width
                    height: childrenRect.height

                    Switch {
                        visible: choice.toggle
                        checked: choice.modelData.value === "true"
                        onToggled: AccessPrompt.setChoice(choice.modelData.id, checked ? "false" : "true")
                    }

                    Dropdown {
                        visible: !choice.toggle
                        fieldWidth: 150
                        options: choice.modelData.options
                        value: choice.modelData.value
                        onPicked: v => AccessPrompt.setChoice(choice.modelData.id, v)
                    }
                }
            }
        }

        Item {
            width: 1
            height: 4
        }

        Row {
            anchors.horizontalCenter: parent.horizontalCenter
            spacing: 10

            DialogButton {
                text: AccessPrompt.denyLabel
                onClicked: AccessPrompt.answer(false)
            }

            DialogButton {
                text: AccessPrompt.grantLabel
                primary: true
                onClicked: AccessPrompt.answer(true)
            }
        }
    }
}
