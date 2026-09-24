import QtQuick
import Atrium.Shell
import Atrium
import shell.modules.bar
import shell.modules.clipboard
import shell.modules.controlcenter
import shell.modules.desktop
import shell.modules.dock
import shell.modules.emoji
import shell.modules.launcher
import shell.modules.notifications
import shell.modules.osd
import shell.modules.polkit
import shell.modules.session
import shell.modules.windowmenu

// atrium's desktop shell.
ShellRoot {
    Variants {
        model: Shell.screens

        Bar {
            required property ShellScreen modelData

            screen: modelData
        }
    }

    Variants {
        model: Shell.screens

        WallpaperLayer {
            required property ShellScreen modelData

            screen: modelData
        }
    }

    Variants {
        model: Shell.screens

        Desktop {
            required property ShellScreen modelData

            screen: modelData
        }
    }

    Variants {
        model: Shell.screens

        Dock {
            required property ShellScreen modelData

            screen: modelData
        }
    }

    Launcher {}

    ClipboardPicker {}

    EmojiPicker {}

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
            if (name === "welcome")
                Shell.launch("welcome.qml", {});
            if (name !== "settings" && !name.startsWith("settings:"))
                return;
            Shell.launch("settings.qml", { ATRIUM_SETTINGS_PAGE: name.slice(9) });
        }
    }

    // The first login: the welcome, once.
    Component.onCompleted: if (Welcome.due) Shell.launch("welcome.qml", {})

    About {}

    SessionConfirm {}
}
