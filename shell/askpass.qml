//@ pragma AppId atrium-askpass

import QtQuick
import Atrium.Shell
import Atrium
import shell.modules.polkit
import shell.services

// The password for sudo -A and ssh (SUDO_ASKPASS, SSH_ASKPASS: atrium-askpass),
// asked the way the polkit agent asks: the password goes to standard output,
// Cancel exits 1. The prompt they pass comes in ATRIUM_ASKPASS_PROMPT.
ShellRoot {
    id: root

    readonly property string prompt: Shell.env("ATRIUM_ASKPASS_PROMPT")
    // "[sudo] password for mkonic: " says who; ssh's own prompts say what.
    readonly property string user: (/password for ([^:\s]+)/.exec(prompt) ?? [])[1] ?? Shell.env("USER")
    readonly property bool sudo: prompt.startsWith("[sudo]")
    // The app it was typed in, and the command asking.
    readonly property var asker: Shell.askingProcess()

    PanelWindow {
        screen: Shell.screen(Atrium.focusedOutput?.name)
        anchors {
            top: true
            bottom: true
            left: true
            right: true
        }
        exclusiveZone: -1
        color: Theme.palette.scrim
        WlrLayershell.layer: WlrLayer.Overlay
        WlrLayershell.namespace: "atrium-askpass"
        WlrLayershell.keyboardFocus: WlrKeyboardFocus.Exclusive

        MouseArea {
            anchors.fill: parent  // nothing else until this is answered
        }

        AuthCard {
            anchors.centerIn: parent
            command: root.asker.command ?? ""

            // What AuthCard reads from polkit's flow, for sudo.
            flow: QtObject {
                readonly property string message: root.sudo ? `${root.asker.app ?? "A command"} wants to make changes.`
                    : root.prompt.replace(/:\s*$/, "") || "A password is needed."
                readonly property string supplementaryMessage: root.sudo ? "Enter your password to allow this." : ""
                readonly property bool supplementaryIsError: false
                readonly property string iconName: root.asker.icon ?? "utilities-terminal"
                readonly property bool isResponseRequired: true
                readonly property bool responseVisible: false
                readonly property string inputPrompt: "Password"
                readonly property var identities: []
                readonly property var selectedIdentity: root.sudo ? { "displayName": root.user } : null
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
