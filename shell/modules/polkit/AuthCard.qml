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
    readonly property var identity: flow?.selectedIdentity ?? null
    property bool wrong: false

    width: 360
    height: column.implicitHeight + 48
    radius: 26
    color: Theme.panel(Theme.palette.m3SurfaceContainer, 0.92)

    GlassRim {}

    border.width: 1
    border.color: Theme.alpha(Theme.palette.m3Outline, 0.22)

    layer.enabled: true
    layer.effect: MultiEffect {
        shadowEnabled: true
        shadowColor: Qt.rgba(0, 0, 0, 0.5)
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
                color: Theme.palette.m3Primary

                MaterialIcon {
                    anchors.centerIn: parent
                    text: "admin_panel_settings"
                    fill: 1
                    font.pointSize: 24
                    color: Theme.palette.m3OnPrimary
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
                color: Theme.palette.m3SurfaceContainerHigh

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
            font.weight: Font.DemiBold
        }

        StyledText {
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
            text: root.wrong ? "Wrong password. Try again."
                : root.flow?.supplementaryMessage ? root.flow.supplementaryMessage
                : "Enter the password to allow this."
            font.pointSize: Theme.font.size.small
            color: root.wrong || root.flow?.supplementaryIsError ? "#ffb4ab" : Theme.palette.m3OnSurfaceVariant
        }

        Item {
            width: 1
            height: 2
        }

        // Whose password; with more than one administrator, click to switch.
        Rectangle {
            width: parent.width
            height: 38
            radius: 12
            color: Theme.alpha(Theme.palette.m3OnSurface, 0.07)

            MaterialIcon {
                id: person

                anchors.left: parent.left
                anchors.leftMargin: 12
                anchors.verticalCenter: parent.verticalCenter
                text: "person"
                font.pointSize: Theme.font.size.normal
                color: Theme.palette.m3OnSurfaceVariant
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
                color: Theme.palette.m3OnSurfaceVariant
            }

            MouseArea {
                anchors.fill: parent
                enabled: swap.visible
                onClicked: root.flow.selectNextIdentity()
            }
        }

        Rectangle {
            width: parent.width
            height: 38
            radius: 12
            color: Theme.palette.m3SurfaceContainerHigh
            border.width: 1
            border.color: root.wrong ? "#ffb4ab" : Theme.alpha(Theme.palette.m3Primary, password.activeFocus ? 0.8 : 0.3)

            TextInput {
                id: password

                anchors.fill: parent
                anchors.leftMargin: 12
                anchors.rightMargin: 12
                verticalAlignment: TextInput.AlignVCenter
                echoMode: root.flow?.responseVisible ? TextInput.Normal : TextInput.Password
                enabled: root.flow?.isResponseRequired ?? false
                color: Theme.palette.m3OnSurface
                font.family: Theme.font.sans
                font.pointSize: Theme.font.size.normal
                clip: true
                focus: true
                onAccepted: root.submit()
                Keys.onEscapePressed: root.flow?.cancelAuthenticationRequest()

                StyledText {
                    anchors.verticalCenter: parent.verticalCenter
                    visible: !password.text
                    text: (root.flow?.inputPrompt ?? "").replace(/:\s*$/, "") || "Password"
                    color: Theme.alpha(Theme.palette.m3OnSurfaceVariant, 0.7)
                    font.pointSize: Theme.font.size.small
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
                text: "Cancel"
                onClicked: root.flow?.cancelAuthenticationRequest()
            }

            DialogButton {
                text: "OK"
                primary: true
                enabled: (root.flow?.isResponseRequired ?? false) && password.text.length > 0
                onClicked: root.submit()
            }
        }
    }

    component DialogButton: Rectangle {
        id: button

        property string text
        property bool primary: false
        signal clicked

        width: 136
        height: 36
        radius: 18
        opacity: enabled ? 1 : 0.5
        color: primary ? Theme.palette.m3Primary
                       : (area.containsMouse ? Theme.alpha(Theme.palette.m3OnSurface, 0.16) : Theme.alpha(Theme.palette.m3OnSurface, 0.1))

        StyledText {
            anchors.centerIn: parent
            text: button.text
            font.weight: Font.DemiBold
            color: button.primary ? Theme.palette.m3OnPrimary : Theme.palette.m3OnSurface
        }

        MouseArea {
            id: area

            anchors.fill: parent
            hoverEnabled: true
            onClicked: button.clicked()
        }
    }
}
