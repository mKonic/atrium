pragma ComponentBehavior: Bound

import QtQuick
import Atrium.Shell
import Atrium
import shell.components
import shell.services
import shell.modules.bar

// The login screen, as macOS draws it: the time at the top, the people who
// can log in along the bottom, a password field under the chosen one, and
// Sleep, Restart and Shut Down below. Only the first screen takes the
// keyboard; the others show the same backdrop and clock.
PanelWindow {
    id: root

    readonly property bool primary: screen === Shell.screens[0]
    // With AccountsService, pick from its people; without, type a name.
    readonly property var people: Accounts.others
    // Who logged in last, and into what, preselected (as SDDM does).
    readonly property var last: Session.lastLogin()
    property int chosen: Math.max(0, people.findIndex(p => p.userName === last.user))
    readonly property var person: people[chosen] ?? null
    readonly property var sessions: Session.waylandSessions()
    // The desktop this person used last, unless another was picked here.
    property string pickedSession: ""
    readonly property string sessionId: pickedSession || Session.sessionFor(userName())
    readonly property int sessionIndex: Math.max(0, sessions.findIndex(s => s.id === sessionId))
    readonly property var session: sessions[sessionIndex] ?? null
    readonly property string message: Greeter.message
    readonly property bool busy: Greeter.busy

    onChosenChanged: pickedSession = ""

    // Where typing starts: the name, unless one is already there (the last
    // one's, or a person picked), then the password. Once: a binding would
    // move it out of the name at its first letter.
    Component.onCompleted: {
        if (!primary)
            return;
        if (person === null && nameField.text.length === 0)
            nameField.forceActiveFocus();
        else
            password.forceActiveFocus();
    }

    function userName(): string {
        return person ? person.userName : nameField.text.trim();
    }

    function submit(): void {
        if (!busy && userName().length > 0)
            Greeter.login(userName(), password.text, session ?? {});
    }

    anchors {
        top: true
        bottom: true
        left: true
        right: true
    }
    exclusionMode: ExclusionMode.Ignore
    color: "transparent"
    WlrLayershell.layer: WlrLayer.Overlay
    WlrLayershell.namespace: "atrium-greeter"
    WlrLayershell.keyboardFocus: primary ? WlrKeyboardFocus.Exclusive : WlrKeyboardFocus.None

    Connections {
        target: Greeter
        enabled: root.primary

        function onFailed(): void {
            password.text = "";
            shake.restart();
        }
    }

    // A calm backdrop in the theme's colours: there is no wallpaper before
    // anyone has logged in.
    Rectangle {
        anchors.fill: parent
        gradient: Gradient {
            GradientStop { position: 0.0; color: Qt.tint(Theme.dark.controlBackground, Theme.dark.accentFill) }
            GradientStop { position: 0.55; color: Theme.dark.controlBackground }
            GradientStop { position: 1.0; color: Qt.tint(Theme.dark.controlBackground, Theme.dark.accentFill) }
        }
    }

    // A press on the backdrop closes the Control Center.
    MouseArea {
        anchors.fill: parent
        onPressed: Panels.open = ""
    }

    SystemClock {
        id: clock

        precision: SystemClock.Minutes
    }

    Column {
        anchors.horizontalCenter: parent.horizontalCenter
        y: Math.round(parent.height * 0.1)
        spacing: 0

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

    // Top right, as on a Mac's login screen: the keyboard layout, the
    // battery, and Wi-Fi (opening the Control Center).
    Row {
        visible: root.primary
        anchors.top: parent.top
        anchors.right: parent.right
        anchors.margins: 8
        spacing: Theme.spacing.small

        InputMenu {
            bar: root
            greeter: true
        }

        BatteryIndicator {
            greeter: true
        }

        Pill {
            implicitWidth: network.implicitWidth + Theme.padding.normal * 2

            MaterialIcon {
                id: network

                anchors.centerIn: parent
                text: Network.glyph
                font.pointSize: Theme.font.size.normal
                color: Network.online ? Theme.palette.label : Theme.palette.tertiaryLabel
            }

            TapHandler {
                onTapped: Panels.toggle("control")
            }
        }
    }

    // The people, the chosen one large with the password field under them.
    Column {
        visible: root.primary
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: power.top
        anchors.bottomMargin: 56
        spacing: 14

        Row {
            anchors.horizontalCenter: parent.horizontalCenter
            spacing: 18
            visible: root.people.length > 1

            Repeater {
                model: root.people

                Avatar {
                    id: other

                    required property var modelData
                    required property int index

                    user: modelData
                    size: 44
                    opacity: index === root.chosen ? 1 : 0.55

                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            root.chosen = other.index;
                            Greeter.clearMessage();
                            password.text = "";
                            password.forceActiveFocus();
                        }
                    }
                }
            }
        }

        Avatar {
            anchors.horizontalCenter: parent.horizontalCenter
            visible: root.person !== null
            user: root.person ?? {}
            size: 96
        }

        StyledText {
            anchors.horizontalCenter: parent.horizontalCenter
            visible: root.person !== null
            text: root.person ? (root.person.realName || root.person.userName) : ""
            font.pointSize: 15
            font.weight: Font.DemiBold
            color: Theme.dark.label
        }

        FieldPill {
            id: nameBox

            visible: root.person === null

            TextInput {
                id: nameField

                selectionColor: Theme.palette.accent
                selectedTextColor: Theme.palette.labelOnAccent
                anchors.fill: parent
                anchors.leftMargin: 16
                anchors.rightMargin: 16
                verticalAlignment: TextInput.AlignVCenter
                color: Theme.dark.label
                font.pointSize: 12
                text: root.last.user ?? ""
                onAccepted: password.forceActiveFocus()

                StyledText {
                    anchors.verticalCenter: parent.verticalCenter
                    visible: nameField.text.length === 0 && nameField.preeditText.length === 0
                    text: qsTr("Name")
                    color: Theme.dark.secondaryLabel
                }
            }
        }

        FieldPill {
            id: passwordBox

            TextInput {
                id: password

                selectionColor: Theme.palette.accent
                selectedTextColor: Theme.palette.labelOnAccent
                anchors.fill: parent
                anchors.leftMargin: 16
                anchors.rightMargin: 40
                verticalAlignment: TextInput.AlignVCenter
                color: Theme.dark.label
                font.pointSize: 12
                echoMode: TextInput.Password
                passwordCharacter: "●"
                enabled: !root.busy
                onAccepted: root.submit()
                // Empty, beside a running session: back to it.
                Keys.onEscapePressed: {
                    if (text.length > 0)
                        text = "";
                    else
                        Greeter.goBack();
                }
                // Left and right pick someone else, before anything is typed.
                Keys.onLeftPressed: event => {
                    if (text.length > 0 || root.people.length < 2)
                        event.accepted = false;
                    else
                        root.chosen = (root.chosen + root.people.length - 1) % root.people.length;
                }
                Keys.onRightPressed: event => {
                    if (text.length > 0 || root.people.length < 2)
                        event.accepted = false;
                    else
                        root.chosen = (root.chosen + 1) % root.people.length;
                }

                StyledText {
                    anchors.verticalCenter: parent.verticalCenter
                    visible: password.text.length === 0
                    text: qsTr("Enter Password")
                    color: Theme.dark.secondaryLabel
                }
            }

            MaterialIcon {
                anchors.right: parent.right
                anchors.rightMargin: 10
                anchors.verticalCenter: parent.verticalCenter
                visible: !root.busy
                text: "arrow_forward"
                color: Theme.dark.label
                opacity: password.text.length > 0 ? 1 : 0.4

                MouseArea {
                    anchors.fill: parent
                    anchors.margins: -6
                    onClicked: root.submit()
                }
            }

            // Turning while greetd checks.
            MaterialIcon {
                anchors.right: parent.right
                anchors.rightMargin: 10
                anchors.verticalCenter: parent.verticalCenter
                visible: root.busy
                text: "progress_activity"
                color: Theme.dark.label

                RotationAnimation on rotation {
                    running: root.busy
                    from: 0
                    to: 360
                    duration: 900
                    loops: Animation.Infinite
                }
            }

            SequentialAnimation {
                id: shake

                loops: 1
                NumberAnimation { target: passwordBox; property: "anchors.horizontalCenterOffset"; to: -12; duration: 50 * Theme.anim.factor }
                NumberAnimation { target: passwordBox; property: "anchors.horizontalCenterOffset"; to: 12; duration: 70 * Theme.anim.factor }
                NumberAnimation { target: passwordBox; property: "anchors.horizontalCenterOffset"; to: -8; duration: 60 * Theme.anim.factor }
                NumberAnimation { target: passwordBox; property: "anchors.horizontalCenterOffset"; to: 0; duration: 50 * Theme.anim.factor }
            }
        }

        StyledText {
            anchors.horizontalCenter: parent.horizontalCenter
            height: 18
            text: root.message
            font.pointSize: Theme.font.size.small
            color: Theme.dark.label
        }
    }

    // Sleep, Restart, Shut Down; and which desktop to start, when there are
    // several.
    Row {
        id: power

        visible: root.primary
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 36
        spacing: 36

        Repeater {
            model: (Greeter.canGoBack ? [{ icon: "arrow_back", text: qsTr("Back"), action: "back" }] : []).concat([
                { icon: "bedtime", text: qsTr("Sleep"), action: "sleep" },
                { icon: "restart_alt", text: qsTr("Restart"), action: "restart" },
                { icon: "power_settings_new", text: qsTr("Shut Down"), action: "shutdown" }
            ])

            Column {
                id: button

                required property var modelData

                spacing: 6

                Rectangle {
                    anchors.horizontalCenter: parent.horizontalCenter
                    width: 44
                    height: 44
                    radius: 22
                    color: powerArea.containsMouse ? Theme.dark.fill : Theme.dark.tertiaryFill

                    MaterialIcon {
                        anchors.centerIn: parent
                        text: button.modelData.icon
                        color: Theme.dark.label
                        font.pointSize: 16
                    }

                    MouseArea {
                        id: powerArea

                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: button.modelData.action === "back" ? Greeter.goBack() : Session.now(button.modelData.action)
                    }
                }

                StyledText {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: button.modelData.text
                    font.pointSize: Theme.font.size.small
                    color: Theme.dark.label
                }
            }
        }
    }

    StyledText {
        visible: root.primary && root.sessions.length > 1
        anchors.left: parent.left
        anchors.bottom: parent.bottom
        anchors.margins: 24
        text: (root.session?.name ?? "") + "  ▾"
        font.pointSize: Theme.font.size.small
        color: Theme.dark.label

        MouseArea {
            anchors.fill: parent
            anchors.margins: -6
            cursorShape: Qt.PointingHandCursor
            onClicked: root.pickedSession = root.sessions[(root.sessionIndex + 1) % root.sessions.length].id
        }
    }
}
