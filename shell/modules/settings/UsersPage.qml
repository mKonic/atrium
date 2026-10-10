pragma ComponentBehavior: Bound

import QtCore
import QtQuick
import QtQuick.Effects
import shell.components
import shell.services
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
        color: Theme.palette.accentFill
        clip: true

        StyledText {
            anchors.centerIn: parent
            visible: !(avatar.user.icon ?? "")
            text: avatar.user.initials ?? ""
            font.pointSize: avatar.size * 0.3
            font.weight: Font.DemiBold
            color: Theme.palette.label
        }

        Image {
            anchors.fill: parent
            visible: !!(avatar.user.icon ?? "")
            source: avatar.user.icon ?? ""
            fillMode: Image.PreserveAspectCrop
            sourceSize: Qt.size(avatar.size * 2, avatar.size * 2)
            // clip only cuts to the square: the circle masks it.
            layer.enabled: true
            layer.effect: MultiEffect {
                maskEnabled: true
                maskSource: circle
                maskThresholdMin: 0.5
                maskSpreadAtMin: 1
            }
        }

        Rectangle {
            id: circle

            anchors.fill: parent
            radius: width / 2
            visible: false
            layer.enabled: true
        }
    }

    MissingNote {
        message: Accounts.available ? "" : qsTr("AccountsService isn't running.")
        explanation: qsTr("User accounts are listed and changed through AccountsService.")
    }

    // You.
    Rectangle {
        visible: Accounts.available
        width: parent.width
        height: 150
        radius: 14
        color: Theme.palette.groupedBackground
        border.width: 1
        border.color: Theme.palette.separator

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
                color: Theme.palette.scrim

                StyledText {
                    anchors.centerIn: parent
                    text: qsTr("Edit")
                    font.pointSize: Theme.font.size.smaller
                    color: Theme.dark.label
                }
            }
        }

        PillButton {
            anchors.right: parent.right
            anchors.rightMargin: 20
            anchors.verticalCenter: parent.verticalCenter
            text: qsTr("Change Password…")
            onClicked: passwordSheet.open()
        }

        Column {
            anchors.left: mine.right
            anchors.leftMargin: 20
            anchors.verticalCenter: parent.verticalCenter
            spacing: 8

            TextControl {
                fieldWidth: 260
                placeholder: qsTr("Full name")
                value: Accounts.me.realName ?? ""
                onCommitted: v => Accounts.setRealName(v)
            }

            Row {
                spacing: 8

                StyledText {
                    anchors.verticalCenter: parent.verticalCenter
                    text: Accounts.me.userName ?? ""
                    color: Theme.palette.secondaryLabel
                }

                Rectangle {
                    anchors.verticalCenter: parent.verticalCenter
                    visible: Accounts.me.admin ?? false
                    width: adminLabel.implicitWidth + 14
                    height: 20
                    radius: 10
                    color: Theme.palette.accentFill

                    StyledText {
                        id: adminLabel

                        anchors.centerIn: parent
                        text: qsTr("Admin")
                        font.pointSize: Theme.font.size.smaller
                        color: Theme.palette.label
                    }
                }
            }
        }
    }

    // Everyone else.
    Group {
        visible: Accounts.available
        title: qsTr("Other users")
        headerActions: [
            PillButton {
                visible: Accounts.me.admin ?? false
                text: qsTr("Add User…")
                icon: "person_add"
                onClicked: addSheet.open()
            }
        ]

        StyledText {
            visible: Accounts.others.length === 0
            padding: 10
            text: qsTr("Nobody else has an account on this computer.")
            color: Theme.palette.secondaryLabel
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
                        text: row.modelData.userName + (row.modelData.admin ? qsTr(" · Admin") : "")
                        font.pointSize: Theme.font.size.smaller
                        color: Theme.palette.secondaryLabel
                    }
                }
            }
        }
    }

    Sheet {
        id: passwordSheet

        title: qsTr("Change Password")
        action: "Change Password"
        ready: next.text.length > 0 && next.text === verify.text && current.text.length > 0
        onOpened: {
            current.text = next.text = verify.text = "";
            current.focusField();
        }
        onSubmitted: {
            busy = true;
            error = "";
            Accounts.changePassword(current.text, next.text);
        }

        Field {
            id: current

            width: parent.width
            password: true
            placeholder: qsTr("Current password")
            onAccepted: next.focusField()
        }

        Field {
            id: next

            width: parent.width
            password: true
            placeholder: qsTr("New password")
            onAccepted: verify.focusField()
        }

        Field {
            id: verify

            width: parent.width
            password: true
            placeholder: qsTr("Verify")
            onAccepted: passwordSheet.submitted()
        }

        StyledText {
            visible: verify.text.length > 0 && verify.text !== next.text
            text: qsTr("The new passwords don't match.")
            font.pointSize: Theme.font.size.smaller
            color: Theme.palette.secondaryLabel
        }
    }

    Sheet {
        id: addSheet

        property bool nameEdited: false

        title: qsTr("New User")
        action: "Create User"
        // An account with no password would let anyone in.
        ready: fullName.text.trim().length > 0 && Accounts.validUserName(account.text) && newPassword.text.length > 0
               && newPassword.text === newVerify.text
        onOpened: {
            fullName.text = account.text = newPassword.text = newVerify.text = "";
            nameEdited = false;
            adminSwitch.checked = false;
            fullName.focusField();
        }
        onSubmitted: {
            busy = true;
            error = "";
            Accounts.addUser(fullName.text, account.text, newPassword.text, adminSwitch.checked);
        }

        Field {
            id: fullName

            width: parent.width
            placeholder: qsTr("Full name")
            onTextChanged: if (!addSheet.nameEdited) account.text = Accounts.suggestUserName(text)
        }

        Field {
            id: account

            width: parent.width
            placeholder: qsTr("Account name")
            onTextChanged: if (activeFocus) addSheet.nameEdited = true
        }

        Field {
            id: newPassword

            width: parent.width
            password: true
            placeholder: qsTr("Password")
        }

        Field {
            id: newVerify

            width: parent.width
            password: true
            placeholder: qsTr("Verify")
        }

        Row {
            spacing: 10

            Switch {
                name: qsTr("Allow this user to administer this computer")
                id: adminSwitch
                onToggled: checked = !checked
            }

            StyledText {
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("Allow this user to administer this computer")
                font.pointSize: Theme.font.size.small
            }
        }
    }

    FilePicker {
        id: picker

        title: qsTr("Choose a Picture")
        folder: StandardPaths.writableLocation(StandardPaths.PicturesLocation)
        nameFilter: qsTr("Pictures (*.png *.jpg *.jpeg *.webp *.gif)")
        onPicked: file => Accounts.setPicture(file.toString())
    }

    Connections {
        target: Accounts

        function onFailed(why: string): void {
            console.warn("accounts:", why);
        }

        function onPasswordChanged(ok: bool, message: string): void {
            passwordSheet.busy = false;
            if (ok)
                passwordSheet.close();
            else
                passwordSheet.error = message || "The password wasn't changed.";
        }

        function onUserAdded(ok: bool, message: string): void {
            addSheet.busy = false;
            if (ok)
                addSheet.close();
            else
                addSheet.error = message || "The user wasn't created.";
        }
    }
}
