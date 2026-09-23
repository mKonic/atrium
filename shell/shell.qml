//@ pragma UseQApplication

import QtQuick
import Quickshell
import qs.modules.bar
import qs.modules.desktop
import qs.modules.dock

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
}
