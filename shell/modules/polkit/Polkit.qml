import QtQuick
import Quickshell
import Quickshell.Services.Polkit
import Quickshell.Wayland
import Atrium

// The session's polkit agent: when an app asks for administrator rights
// (pkexec, a settings change), this asks for the password. If another agent
// already serves the session, it steps aside.
Scope {
    PolkitAgent {
        id: agent
    }

    PanelWindow {
        id: window

        readonly property var flow: agent.flow

        visible: agent.isActive && flow !== null && !flow.isCompleted
        screen: Quickshell.screens.find(s => s.name === Atrium.focusedOutput?.name) ?? Quickshell.screens[0]
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
            flow: window.flow
        }
    }
}
