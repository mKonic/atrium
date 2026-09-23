pragma ComponentBehavior: Bound

import QtCore
import QtQuick
import QtQuick.Dialogs
import qs.components
import qs.services
import Atrium

// Users & Groups: you (picture, name, whether you're an admin) and the
// other people with an account here. Changes go through AccountsService.
Column {
    id: root

    spacing: 20

    component Avatar: Rectangle {
        id: avatar

        property var user: ({})
        property real size: 40

        width: size
        height: size
        radius: size / 2
        color: Theme.palette.m3SecondaryContainer
        clip: true

        StyledText {
            anchors.centerIn: parent
            visible: !(avatar.user.icon ?? "")
            text: avatar.user.initials ?? ""
            font.pointSize: avatar.size * 0.3
            font.weight: Font.DemiBold
            color: Theme.palette.m3OnSecondaryContainer
        }

        Image {
            anchors.fill: parent
            visible: !!(avatar.user.icon ?? "")
            source: avatar.user.icon ?? ""
            fillMode: Image.PreserveAspectCrop
            sourceSize: Qt.size(avatar.size * 2, avatar.size * 2)
            layer.enabled: true
        }
    }

    StyledText {
        visible: !Accounts.available
        text: "User accounts aren't available (AccountsService isn't running)."
        color: Theme.palette.m3OnSurfaceVariant
    }

    // You.
    Rectangle {
        visible: Accounts.available
        width: parent.width
        height: 150
        radius: 14
        color: Theme.palette.m3SurfaceContainer
        border.width: 1
        border.color: Theme.alpha(Theme.palette.m3Outline, 0.12)

        Avatar {
            id: mine

            x: 24
            anchors.verticalCenter: parent.verticalCenter
            size: 88
            user: Accounts.me

            MouseArea {
                id: pictureArea

                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: picker.open()
            }

            Rectangle {
                anchors.bottom: parent.bottom
                width: parent.width
                height: 26
                visible: pictureArea.containsMouse
                color: Qt.rgba(0, 0, 0, 0.55)

                StyledText {
                    anchors.centerIn: parent
                    text: "Edit"
                    font.pointSize: Theme.font.size.smaller
                    color: "white"
                }
            }
        }

        Column {
            anchors.left: mine.right
            anchors.leftMargin: 20
            anchors.verticalCenter: parent.verticalCenter
            spacing: 8

            TextControl {
                fieldWidth: 260
                placeholder: "Full name"
                value: Accounts.me.realName ?? ""
                onCommitted: v => Accounts.setRealName(v)
            }

            Row {
                spacing: 8

                StyledText {
                    anchors.verticalCenter: parent.verticalCenter
                    text: Accounts.me.userName ?? ""
                    color: Theme.palette.m3OnSurfaceVariant
                }

                Rectangle {
                    anchors.verticalCenter: parent.verticalCenter
                    visible: Accounts.me.admin ?? false
                    width: adminLabel.implicitWidth + 14
                    height: 20
                    radius: 10
                    color: Theme.palette.m3PrimaryContainer

                    StyledText {
                        id: adminLabel

                        anchors.centerIn: parent
                        text: "Admin"
                        font.pointSize: Theme.font.size.smaller
                        color: Theme.palette.m3OnPrimaryContainer
                    }
                }
            }
        }
    }

    // Everyone else.
    Group {
        visible: Accounts.available
        title: "Other users"

        StyledText {
            visible: Accounts.others.length === 0
            padding: 10
            text: "Nobody else has an account on this computer."
            color: Theme.palette.m3OnSurfaceVariant
        }

        Repeater {
            model: Accounts.others

            Item {
                id: row

                required property var modelData

                width: parent?.width ?? 0
                height: 52

                Avatar {
                    anchors.verticalCenter: parent.verticalCenter
                    size: 36
                    user: row.modelData
                }

                Column {
                    x: 50
                    anchors.verticalCenter: parent.verticalCenter

                    StyledText {
                        text: row.modelData.realName || row.modelData.userName
                    }

                    StyledText {
                        text: row.modelData.userName + (row.modelData.admin ? " · Admin" : "")
                        font.pointSize: Theme.font.size.smaller
                        color: Theme.palette.m3OnSurfaceVariant
                    }
                }
            }
        }
    }

    FileDialog {
        id: picker

        title: "Choose a picture"
        currentFolder: StandardPaths.writableLocation(StandardPaths.PicturesLocation)
        nameFilters: ["Pictures (*.png *.jpg *.jpeg *.webp *.gif)"]
        onAccepted: Accounts.setPicture(selectedFile.toString())
    }

    Connections {
        target: Accounts

        function onFailed(why: string): void {
            console.warn("accounts:", why);
        }
    }
}
