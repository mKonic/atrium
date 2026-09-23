import QtQuick
import Quickshell
import Quickshell.Wayland
import qs.components
import qs.services
import Atrium

// The menu under the bar's logo, as the Apple menu is laid out.
PanelWindow {
    id: root

    visible: Panels.open === "system"
    screen: Quickshell.screens.find(s => s.name === Atrium.focusedOutput?.name) ?? Quickshell.screens[0]
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
    WlrLayershell.keyboardFocus: visible ? WlrKeyboardFocus.OnDemand : WlrKeyboardFocus.None

    Connections {
        target: Atrium

        function onFocusedWindowChanged(): void {
            // A window taking focus means a click elsewhere.
            if (!Atrium.focusedWindow)
                return;
            if (Panels.open === "system")
                Panels.open = "";
        }
    }

    Menu {
        id: menu

        focus: true
        Keys.onEscapePressed: Panels.open = ""
        actions: [
            { icon: "info", text: "About This Computer", run: () => Panels.open = "about" },
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
