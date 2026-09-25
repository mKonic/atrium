import QtQuick
import Atrium.Shell
import shell.components
import shell.services
import Atrium

// The menu under the bar's logo, as the Apple menu is laid out.
PanelWindow {
    id: root

    visible: Panels.open === "system"
    screen: Shell.screen(Atrium.focusedOutput?.name)
    anchors {
        top: true
        left: true
    }
    margins {
        top: 8
        left: 8
    }
    implicitWidth: menu.width
    implicitHeight: menu.height
    exclusiveZone: 0
    color: "transparent"
    WlrLayershell.layer: WlrLayer.Top
    WlrLayershell.namespace: "atrium-system-menu"
    WlrLayershell.keyboardFocus: visible ? WlrKeyboardFocus.Exclusive : WlrKeyboardFocus.None

    Menu {
        id: menu

        focus: true
        Keys.onEscapePressed: Panels.open = ""
        actions: [
            { icon: "info", text: "About This Computer", run: () => Panels.open = "about" },
            "-",
            { icon: "settings", text: "System Settings…", run: () => Atrium.action("shell", "settings") },
            "-",
            { icon: "bedtime", text: "Sleep", run: () => Session.request("sleep") },
            { icon: "restart_alt", text: "Restart…", run: () => Session.request("restart") },
            { icon: "power_settings_new", text: "Shut Down…", run: () => Session.request("shutdown") },
            "-",
            { icon: "logout", text: `Log Out ${SystemInfo.user}…`, run: () => Session.request("logout") }
        ]
        onPicked: if (Panels.open === "system") Panels.open = ""
    }
}
