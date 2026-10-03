pragma ComponentBehavior: Bound

import QtQuick
import Atrium.Shell
import shell.components
import shell.modules.settings
import shell.services
import Atrium

// Open With: the apps that can open it, the last one used first and
// picked. Double-click, or pick one and Open; closing it or Cancel opens
// nothing.
FloatingWindow {
    id: root

    property string chosen: AppChooser.last || (AppChooser.choices[0] ?? "")

    title: "Open With"
    color: Theme.palette.windowBackground
    implicitWidth: 440
    implicitHeight: 520
    minimumSize: Qt.size(360, 360)
    onVisibleChanged: if (!visible) AppChooser.cancel()

    Shortcut {
        sequence: "Escape"
        onActivated: AppChooser.cancel()
    }

    Item {
        anchors.fill: parent
        anchors.margins: 20

        Column {
            id: heading

            width: parent.width
            spacing: 4

            StyledText {
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                elide: Text.ElideMiddle
                text: AppChooser.subject ? `Open “${AppChooser.subject}” with` : "Open with"
                font.pointSize: Theme.font.size.large
                font.weight: Font.Bold
            }

            StyledText {
                visible: AppChooser.kind !== ""
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                text: AppChooser.kind
                color: Theme.palette.secondaryLabel
                font.pointSize: Theme.font.size.small
            }
        }

        Rectangle {
            anchors.top: heading.bottom
            anchors.topMargin: 16
            anchors.bottom: footer.top
            anchors.bottomMargin: 16
            width: parent.width
            radius: 10
            color: Theme.palette.groupedBackground
            border.width: 1
            border.color: Theme.palette.separator
            clip: true

            StyledText {
                visible: AppChooser.choices.length === 0
                anchors.centerIn: parent
                text: "No app here can open it."
                color: Theme.palette.secondaryLabel
            }

            ListView {
                id: list

                anchors.fill: parent
                anchors.margins: 4
                clip: true
                focus: true
                boundsBehavior: Flickable.StopAtBounds
                model: AppChooser.apps
                currentIndex: AppChooser.choices.indexOf(root.chosen)
                onCurrentIndexChanged: if (currentIndex >= 0) root.chosen = AppChooser.choices[currentIndex]
                keyNavigationEnabled: true
                Keys.onReturnPressed: AppChooser.choose(root.chosen)
                Keys.onEnterPressed: AppChooser.choose(root.chosen)

                delegate: Rectangle {
                    id: app

                    required property var modelData
                    required property int index
                    readonly property bool picked: root.chosen === modelData.id

                    width: list.width
                    height: 44
                    radius: 7
                    color: picked ? Theme.palette.accent : appArea.containsMouse ? Theme.palette.tertiaryFill : "transparent"

                    IconImage {
                        x: 10
                        anchors.verticalCenter: parent.verticalCenter
                        implicitSize: 30
                        source: Shell.iconPath(app.modelData.icon, "application-x-executable")
                        asynchronous: true
                    }

                    Column {
                        x: 52
                        width: parent.width - 62
                        anchors.verticalCenter: parent.verticalCenter

                        StyledText {
                            width: parent.width
                            elide: Text.ElideRight
                            text: app.modelData.name
                            color: app.picked ? Theme.palette.labelOnAccent : Theme.palette.label
                        }

                        // Two apps by one name: which one this is.
                        StyledText {
                            visible: app.modelData.detail !== ""
                            width: parent.width
                            elide: Text.ElideRight
                            text: app.modelData.detail
                            font.pointSize: Theme.font.size.small
                            color: app.picked ? Theme.palette.labelOnAccent : Theme.palette.secondaryLabel
                        }
                    }

                    MouseArea {
                        id: appArea

                        anchors.fill: parent
                        hoverEnabled: true
                        onClicked: root.chosen = app.modelData.id
                        onDoubleClicked: AppChooser.choose(app.modelData.id)
                    }
                }
            }
        }

        Row {
            id: footer

            anchors.bottom: parent.bottom
            anchors.right: parent.right
            spacing: 10

            PillButton {
                text: "Cancel"
                onClicked: AppChooser.cancel()
            }

            PillButton {
                primary: true
                enabled: root.chosen !== ""
                opacity: enabled ? 1 : 0.5
                text: "Open"
                onClicked: AppChooser.choose(root.chosen)
            }
        }
    }
}
