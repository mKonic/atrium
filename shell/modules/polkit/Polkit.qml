import QtQuick
import Atrium.Shell
import Atrium

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
        color: Qt.rgba(0, 0, 0, 0.35)
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
