import QtQuick
import Atrium.Shell
import Atrium
import shell.services

// The session's polkit agent: when an app asks for administrator rights
// (pkexec, a settings change), this asks for the password. A password that
// worked moments ago answers by itself (PolkitAgent does that, unseen). If
// another agent already serves the session, it steps aside.
Scope {
    PanelWindow {
        id: window

        visible: PolkitAgent.asking
        screen: Shell.screen(Atrium.focusedOutput?.name)
        anchors {
            top: true
            bottom: true
            left: true
            right: true
        }
        exclusiveZone: -1
        // No dimmed screen: the card's glass sees the desktop behind it, as a
        // Mac's password prompt floats on its own.
        color: "transparent"
        WlrLayershell.layer: WlrLayer.Overlay
        WlrLayershell.namespace: "atrium-polkit"
        WlrLayershell.keyboardFocus: visible ? WlrKeyboardFocus.Exclusive : WlrKeyboardFocus.None

        MouseArea {
            anchors.fill: parent  // nothing else until this is answered
        }

        AuthCard {
            anchors.centerIn: parent
            flow: PolkitAgent.flow
        }
    }
}
