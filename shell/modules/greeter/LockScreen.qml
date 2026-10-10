pragma ComponentBehavior: Bound

import QtQuick
import Atrium.Shell
import Atrium
import shell.components
import shell.services

// The lock screen: the login screen's look, for the person who is logged
// in. Every screen has the password field, since the keyboard goes to the
// screen the pointer was on. The compositor keeps everything covered until
// the password is right, also if this crashes (it starts it again).
LockWindow {
    id: root

    readonly property var me: Accounts.me

    function submit(): void {
        if (!Unlock.busy && password.text.length > 0)
            Unlock.tryPassword(password.text);
    }

    color: "black"

    Connections {
        target: Unlock

        function onFailed(): void {
            password.text = "";
            shake.restart();
        }
    }

    Rectangle {
        anchors.fill: parent
        gradient: Gradient {
            GradientStop { position: 0.0; color: Qt.tint(Theme.dark.controlBackground, Theme.dark.accentFill) }
            GradientStop { position: 0.55; color: Theme.dark.controlBackground }
            GradientStop { position: 1.0; color: Qt.tint(Theme.dark.controlBackground, Theme.dark.accentFill) }
        }
    }

    SystemClock {
        id: clock

        precision: SystemClock.Minutes
    }

    Column {
        anchors.horizontalCenter: parent.horizontalCenter
        y: Math.round(parent.height * 0.1)

        StyledText {
            anchors.horizontalCenter: parent.horizontalCenter
            text: Qt.locale().toString(clock.date, "dddd d MMMM")
            font.pointSize: 17
            font.weight: Font.DemiBold
            color: Theme.dark.label
        }

        StyledText {
            anchors.horizontalCenter: parent.horizontalCenter
            text: Qt.locale().toString(clock.date, "hh:mm")
            font.pointSize: 84
            font.weight: Font.Bold
            color: Theme.dark.label
        }
    }

    Column {
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 120
        spacing: 14

        Avatar {
            anchors.horizontalCenter: parent.horizontalCenter
            user: root.me
            size: 96
        }

        StyledText {
            anchors.horizontalCenter: parent.horizontalCenter
            text: root.me.realName || root.me.userName || ""
            font.pointSize: 15
            font.weight: Font.DemiBold
            color: Theme.dark.label
        }

        FieldPill {
            id: passwordBox

            TextInput {
                id: password

                anchors.fill: parent
                anchors.leftMargin: 16
                anchors.rightMargin: 40
                verticalAlignment: TextInput.AlignVCenter
                color: Theme.dark.label
                font.pointSize: 12
                echoMode: TextInput.Password
                passwordCharacter: "●"
                enabled: !Unlock.busy
                focus: true
                onAccepted: root.submit()
                Keys.onEscapePressed: text = ""

                StyledText {
                    anchors.verticalCenter: parent.verticalCenter
                    visible: password.text.length === 0
                    text: Unlock.fingerprint ? qsTr("Fingerprint or Password") : qsTr("Enter Password")
                    color: Theme.dark.secondaryLabel
                }
            }

            MaterialIcon {
                id: submitIcon

                anchors.right: parent.right
                anchors.rightMargin: 10
                anchors.verticalCenter: parent.verticalCenter
                text: Unlock.busy ? "progress_activity" : "arrow_forward"
                color: Theme.dark.label
                opacity: Unlock.busy || password.text.length > 0 ? 1 : 0.4

                RotationAnimation on rotation {
                    running: Unlock.busy
                    from: 0
                    to: 360
                    duration: 900
                    loops: Animation.Infinite
                    onStopped: submitIcon.rotation = 0
                }

                MouseArea {
                    anchors.fill: parent
                    anchors.margins: -6
                    onClicked: root.submit()
                }
            }

            SequentialAnimation {
                id: shake

                NumberAnimation { target: passwordBox; property: "anchors.horizontalCenterOffset"; to: -12; duration: 50 * Theme.anim.factor }
                NumberAnimation { target: passwordBox; property: "anchors.horizontalCenterOffset"; to: 12; duration: 70 * Theme.anim.factor }
                NumberAnimation { target: passwordBox; property: "anchors.horizontalCenterOffset"; to: -8; duration: 60 * Theme.anim.factor }
                NumberAnimation { target: passwordBox; property: "anchors.horizontalCenterOffset"; to: 0; duration: 50 * Theme.anim.factor }
            }
        }

        StyledText {
            anchors.horizontalCenter: parent.horizontalCenter
            height: 18
            text: Unlock.message
            font.pointSize: Theme.font.size.small
            color: Theme.dark.label
        }
    }
}
