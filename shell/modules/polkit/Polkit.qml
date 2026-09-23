import QtQuick
import Quickshell
import Quickshell.Services.Polkit
import Quickshell.Wayland
import Atrium

// The session's polkit agent: when an app asks for administrator rights
// (pkexec, a settings change), this asks for the password. If another agent
// already serves the session, it steps aside.
Scope {
    id: root

    readonly property var flow: agent.flow
    readonly property bool remember: Atrium.settings["security.remember_admin"] ?? true
    property bool answering: false  // replying with the remembered password, unseen

    PolkitAgent {
        id: agent
    }

    // With a password that worked moments ago, answer without asking.
    function tryRemembered(): void {
        const f = flow;
        if (!remember || !f?.isResponseRequired || f.responseVisible || !f.selectedIdentity)
            return;
        const pw = AdminCache.recall(f.selectedIdentity.id);
        if (!pw)
            return;
        answering = true;
        AdminCache.stage(f.selectedIdentity.id, pw);
        f.submit(pw);
    }

    onFlowChanged: {
        answering = false;
        tryRemembered();
    }

    Connections {
        target: root.flow

        function onIsResponseRequiredChanged(): void {
            if (!root.answering)
                root.tryRemembered();
        }

        function onAuthenticationSucceeded(): void {
            if (root.remember)
                AdminCache.commit();
            root.answering = false;
        }

        function onAuthenticationFailed(): void {
            // The remembered one no longer works: forget it and ask.
            if (root.answering)
                AdminCache.forget();
            root.answering = false;
        }

        function onAuthenticationRequestCancelled(): void {
            root.answering = false;
        }
    }

    // Turning the setting off forgets at once.
    onRememberChanged: if (!remember) AdminCache.forget()

    PanelWindow {
        id: window

        readonly property var flow: agent.flow

        visible: agent.isActive && flow !== null && !flow.isCompleted && !root.answering
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
