import QtQuick
import Quickshell
import Quickshell.Wayland
import qs.components
import qs.services
import Atrium

// A window's menu: right-click its title bar (or a GTK header bar). What
// the window's buttons and shortcuts do, in one place.
PanelWindow {
    id: root

    property var window: ({})
    property string outputName
    property point at

    readonly property int windowId: window.id ?? -1
    // As it is now (pinned from the menu a moment ago, say), not as it was.
    readonly property var live: Atrium.windows.find(w => w.id === root.windowId) ?? window

    function open(w: var, output: string, x: int, y: int): void {
        window = w;
        outputName = output;
        at = Qt.point(x, y);
        Panels.open = "window-menu";
    }

    visible: Panels.open === "window-menu"
    onVisibleChanged: if (visible) menu.forceActiveFocus()
    screen: Quickshell.screens.find(s => s.name === root.outputName) ?? Quickshell.screens[0]
    // The whole screen, clear: a click anywhere but the menu closes it.
    anchors {
        top: true
        bottom: true
        left: true
        right: true
    }
    exclusionMode: ExclusionMode.Ignore
    color: "transparent"
    WlrLayershell.layer: WlrLayer.Overlay
    WlrLayershell.namespace: "atrium-window-menu"
    // The keyboard, as a menu has it: Escape closes it at once.
    WlrLayershell.keyboardFocus: visible ? WlrKeyboardFocus.Exclusive : WlrKeyboardFocus.None

    Connections {
        target: Atrium

        function onWindowMenu(window: var, output: string, x: int, y: int): void {
            root.open(window, output, x, y);
        }

        function onFocusedWindowChanged(): void {
            // Another window taking focus means a click elsewhere. (None at
            // all is the menu itself taking the keyboard.)
            if (Panels.open === "window-menu" && Atrium.focusedWindow && Atrium.focusedWindow.id !== root.windowId)
                Panels.open = "";
        }
    }

    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.AllButtons
        onPressed: Panels.open = ""
    }

    Menu {
        id: menu

        // At the pointer, kept on the screen.
        x: Math.max(0, Math.min(root.at.x, root.width - width - 8))
        y: Math.max(0, Math.min(root.at.y, root.height - height - 8))
        focus: true
        width: 250
        title: root.live.title ?? ""
        Keys.onEscapePressed: Panels.open = ""
        actions: [
            { icon: "minimize", text: "Minimize", run: () => Atrium.windowRequest(root.windowId, "minimize") },
            { icon: root.live.maximized ? "close_fullscreen" : "open_in_full",
              text: root.live.maximized ? "Restore" : "Zoom",
              run: () => Atrium.windowRequest(root.windowId, "maximize") },
            { icon: "fullscreen", text: root.live.fullscreen ? "Exit Full Screen" : "Enter Full Screen",
              run: () => Atrium.windowRequest(root.windowId, "fullscreen") },
            "-",
            { icon: "keep", text: root.live.sticky ? "Show on This Space Only" : "Show on All Spaces",
              run: () => Atrium.windowRequest(root.windowId, "pin") },
            { icon: "arrow_back", text: "Move to Previous Space", run: () => Atrium.action("move-to-space-prev") },
            { icon: "arrow_forward", text: "Move to Next Space", run: () => Atrium.action("move-to-space-next") },
            "-",
            { icon: "close", text: "Close", danger: true, run: () => Atrium.closeWindow(root.windowId) }
        ]
        onPicked: if (Panels.open === "window-menu") Panels.open = ""
    }
}
