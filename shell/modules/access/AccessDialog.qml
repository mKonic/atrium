import QtQuick
import Atrium.Shell
import Atrium

// Over everything, the rest dimmed, as the polkit dialog: nothing else until
// it's answered.
PanelWindow {
    visible: true
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
    WlrLayershell.namespace: "atrium-access"
    WlrLayershell.keyboardFocus: WlrKeyboardFocus.Exclusive

    MouseArea {
        anchors.fill: parent
    }

    AccessCard {
        anchors.centerIn: parent
    }
}
