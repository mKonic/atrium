//@ pragma UseQApplication

import QtQuick
import Quickshell
import qs.modules.bar
import qs.modules.clipboard
import qs.modules.controlcenter
import qs.modules.desktop
import qs.modules.dock
import qs.modules.launcher
import qs.modules.notifications
import qs.modules.osd

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
}
