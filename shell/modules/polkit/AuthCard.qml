import QtQuick
import QtQuick.Effects
import Atrium.Shell
import shell.components
import shell.services
import Atrium

// "<App> wants to make changes." with the password field, as macOS asks.
// `flow` is PolkitAgent.flow.
Rectangle {
    id: root

    required property var flow
    // What will run, when the asker says (sudo's command), in small type.
    property string command: ""
    readonly property var identity: flow?.selectedIdentity ?? null
    readonly property bool choosesIdentity: (flow?.identities?.length ?? 0) > 1
    property bool wrong: false

    width: 320
    height: column.implicitHeight + 48
    radius: 26
    color: Theme.material.thick

    Glass {}

    border.width: Theme.lens ? 0 : 1
    border.color: Theme.palette.separator

    layer.enabled: true
    layer.effect: MultiEffect {
        shadowEnabled: !Theme.lens
        shadowColor: Theme.palette.shadow
        shadowBlur: 1
        shadowVerticalOffset: 8
    }

    function submit(): void {
        if (!flow?.isResponseRequired || !password.text)
            return;
        wrong = false;
        flow.submit(password.text);
        password.text = "";
    }

    // It opens as the pickers do: up from a touch smaller, fading in, with
    // the password field ready.
    Component.onCompleted: {
        password.forceActiveFocus();
        shown.restart();
    }

    ParallelAnimation {
        id: shown

        Anim {
            target: root
            property: "scale"
            from: 0.96
            to: 1
            duration: Theme.anim.small
            easing.bezierCurve: Theme.anim.emphasizedDecel
        }
        Anim {
            target: root
            property: "opacity"
            from: 0
            to: 1
            duration: Theme.anim.small
        }
    }

    onFlowChanged: {
        wrong = false;
        password.text = "";
        password.forceActiveFocus();
    }

    Connections {
        target: root.flow

        function onAuthenticationFailed(): void {
            root.wrong = true;
            shake.restart();
        }

        function onIsResponseRequiredChanged(): void {
            if (root.flow.isResponseRequired)
                password.forceActiveFocus();
        }
    }

    // A wrong password shakes the card, as a Mac does.
    SequentialAnimation {
        id: shake

        NumberAnimation {
            target: slide
            property: "x"
            to: -12
            duration: 50
        }
        NumberAnimation {
            target: slide
            property: "x"
            to: 10
            duration: 70
        }
        NumberAnimation {
            target: slide
            property: "x"
            to: -6
            duration: 60
        }
        NumberAnimation {
            target: slide
            property: "x"
            to: 0
            duration: 50
        }
    }

    transform: Translate {
        id: slide
    }

    Column {
        id: column

        anchors.horizontalCenter: parent.horizontalCenter
        y: 24
        width: parent.width - 48
        spacing: 10

        // The asking app's icon, badged with a lock.
        Item {
            anchors.horizontalCenter: parent.horizontalCenter
            width: 64
            height: 64

            IconImage {
                id: appIcon

                anchors.fill: parent
                visible: status === Image.Ready
                source: root.flow?.iconName ? Shell.iconPath(root.flow.iconName, true) : ""
            }

            Rectangle {
                anchors.fill: parent
                visible: !appIcon.visible
                radius: 32
                color: Theme.palette.accent

                MaterialIcon {
                    anchors.centerIn: parent
                    text: "admin_panel_settings"
                    fill: 1
                    font.pointSize: 24
                    color: Theme.palette.labelOnAccent
                }
            }

            Rectangle {
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                anchors.margins: -4
                visible: appIcon.visible
                width: 26
                height: 26
                radius: 13
                color: Theme.material.thick

                Glass {}

                MaterialIcon {
                    anchors.centerIn: parent
                    text: "lock"
                    fill: 1
                    font.pointSize: Theme.font.size.small
                }
            }
        }

        StyledText {
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
            text: root.flow?.message ?? ""
            font.weight: Font.Bold
            font.pointSize: Theme.font.size.larger
        }

        StyledText {
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
            text: root.wrong ? "Wrong password. Try again."
                : root.flow?.supplementaryMessage ? root.flow.supplementaryMessage
                : "Enter the password to allow this."
            font.pointSize: Theme.font.size.small
            color: root.wrong || root.flow?.supplementaryIsError ? Theme.palette.red : Theme.palette.secondaryLabel
        }

        Rectangle {
            anchors.horizontalCenter: parent.horizontalCenter
            visible: root.command !== ""
            width: Math.min(parent.width, commandText.implicitWidth + 20)
            height: 26
            radius: 8
            color: Theme.palette.quaternaryFill

            StyledText {
                id: commandText

                anchors.fill: parent
                anchors.leftMargin: 10
                anchors.rightMargin: 10
                verticalAlignment: Text.AlignVCenter
                horizontalAlignment: Text.AlignHCenter
                text: root.command
                elide: Text.ElideMiddle
                font.family: Theme.font.mono
                font.pointSize: Theme.font.size.small
                color: Theme.palette.secondaryLabel
            }
        }

        Item {
            width: 1
            height: 4
        }

        // Whose password; with more than one administrator, click to switch.
        Rectangle {
            visible: root.choosesIdentity
            width: parent.width
            height: 36
            radius: 10
            color: Theme.palette.tertiaryFill

            MaterialIcon {
                id: person

                anchors.left: parent.left
                anchors.leftMargin: 12
                anchors.verticalCenter: parent.verticalCenter
                text: "person"
                font.pointSize: Theme.font.size.normal
                color: Theme.palette.secondaryLabel
            }

            StyledText {
                anchors.left: person.right
                anchors.leftMargin: 8
                anchors.right: swap.left
                anchors.verticalCenter: parent.verticalCenter
                text: root.identity?.displayName || root.identity?.string || ""
                elide: Text.ElideRight
            }

            MaterialIcon {
                id: swap

                anchors.right: parent.right
                anchors.rightMargin: 10
                anchors.verticalCenter: parent.verticalCenter
                visible: (root.flow?.identities?.length ?? 0) > 1
                text: "unfold_more"
                font.pointSize: Theme.font.size.normal
                color: Theme.palette.secondaryLabel
            }

            MouseArea {
                anchors.fill: parent
                enabled: swap.visible
                onClicked: root.flow.selectNextIdentity()
            }
        }

        Rectangle {
            width: parent.width
            height: 36
            radius: 10
            color: Theme.palette.tertiaryFill
            border.width: root.wrong || password.activeFocus ? 2 : 1
            border.color: root.wrong ? Theme.palette.red : password.activeFocus ? Theme.palette.focusRing : Theme.palette.separator

            TextInput {
                id: password

                anchors.fill: parent
                anchors.leftMargin: 12
                anchors.rightMargin: 12
                verticalAlignment: TextInput.AlignVCenter
                echoMode: root.flow?.responseVisible ? TextInput.Normal : TextInput.Password
                enabled: root.flow?.isResponseRequired ?? false
                color: Theme.palette.label
                font.family: Theme.font.sans
                font.pointSize: Theme.font.size.normal
                clip: true
                focus: true
                onAccepted: root.submit()
                Keys.onEscapePressed: root.flow?.cancelAuthenticationRequest()

                StyledText {
                    anchors.verticalCenter: parent.verticalCenter
                    visible: !password.text
                    // Whose, when there's only the one.
                    text: !root.choosesIdentity && root.identity?.displayName
                        ? `Password for ${root.identity.displayName}`
                        : (root.flow?.inputPrompt ?? "").replace(/:\s*$/, "") || "Password"
                    color: Theme.palette.tertiaryLabel
                    font.pointSize: Theme.font.size.normal
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
                width: (column.width - 10) / 2
                text: "Cancel"
                onClicked: root.flow?.cancelAuthenticationRequest()
            }

            DialogButton {
                width: (column.width - 10) / 2
                text: "Allow"
                primary: true
                enabled: (root.flow?.isResponseRequired ?? false) && password.text.length > 0
                onClicked: root.submit()
            }
        }
    }
}
