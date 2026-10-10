//@ pragma AppId atrium-keyring

import QtQuick
import Atrium.Shell
import Atrium
import shell.modules.polkit
import shell.services

// atrium-keyring's password dialog, asked the way the polkit agent asks: the
// password goes to standard output, Cancel exits 1. ATRIUM_KEYRING_MODE is
// "unlock" (the keyring's password) or "create" (the login password, to make
// it with); ATRIUM_KEYRING_WHO is the app asking; ATRIUM_KEYRING_WRONG is set
// when the last one didn't open it.
ShellRoot {
    id: root

    readonly property bool create: Shell.env("ATRIUM_KEYRING_MODE") === "create"
    readonly property string who: Shell.env("ATRIUM_KEYRING_WHO")
    readonly property bool wrong: Shell.env("ATRIUM_KEYRING_WRONG") !== ""

    PanelWindow {
        screen: Shell.screen(Atrium.focusedOutput?.name)
        anchors {
            top: true
            bottom: true
            left: true
            right: true
        }
        exclusiveZone: -1
        color: "transparent"
        WlrLayershell.layer: WlrLayer.Overlay
        WlrLayershell.namespace: "atrium-keyring"
        WlrLayershell.keyboardFocus: WlrKeyboardFocus.Exclusive

        MouseArea {
            anchors.fill: parent  // nothing else until this is answered
        }

        AuthCard {
            anchors.centerIn: parent
            wrong: root.wrong

            flow: QtObject {
                readonly property string message: root.create ? qsTr("Set up your keyring")
                    : root.who ? `“${root.who}” wants to use your keyring.` : "Unlock your keyring"
                readonly property string supplementaryMessage: root.create
                    ? "Enter your login password. Saved passwords are kept safe with it, and open when you log in."
                    : "Enter the password you log in with. If you changed it, enter the one before."
                readonly property bool supplementaryIsError: false
                readonly property string iconName: "dialog-password"
                readonly property bool isResponseRequired: true
                readonly property bool responseVisible: false
                readonly property string inputPrompt: qsTr("Password")
                readonly property var identities: []
                readonly property var selectedIdentity: null
                signal authenticationFailed

                function submit(text: string): void {
                    Shell.printLine(text);
                    Qt.exit(0);
                }
                function cancelAuthenticationRequest(): void {
                    Qt.exit(1);
                }
                function selectNextIdentity(): void {}
            }
        }
    }
}
