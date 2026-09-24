//@ pragma UseQApplication

import QtQuick
import Quickshell
import Atrium
import qs.modules.bar
import qs.modules.clipboard
import qs.modules.controlcenter
import qs.modules.desktop
import qs.modules.dock
import qs.modules.launcher
import qs.modules.notifications
import qs.modules.osd
import qs.modules.polkit
import qs.modules.session
import qs.modules.windowmenu

// atrium's desktop shell.
ShellRoot {
    Variants {
        model: Quickshell.screens

        Bar {
            required property ShellScreen modelData

            screen: modelData
        }
    }

    Variants {
        model: Quickshell.screens

        Desktop {
            required property ShellScreen modelData

            screen: modelData
        }
    }

    Variants {
        model: Quickshell.screens

        Dock {
            required property ShellScreen modelData

            screen: modelData
        }
    }

    Launcher {}

    ClipboardPicker {}

    Notifications {}

    NotificationCenter {}

    ControlCenter {}

    Osd {}

    Polkit {}

    SystemMenu {}

    WindowMenu {}

    // System Settings runs as its own app (see settings.qml); a running one
    // hears the same action and switches page itself.
    Connections {
        target: Atrium

        function onShellAction(name: string): void {
            if (name !== "settings" && !name.startsWith("settings:"))
                return;
            Quickshell.execDetached(["env", `ATRIUM_SETTINGS_PAGE=${name.slice(9)}`, "qs", "-n", "-p",
                                     `${Quickshell.shellDir}/settings.qml`]);
        }
    }

    About {}

    SessionConfirm {}
}
