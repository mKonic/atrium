//@ pragma UseQApplication

import QtQuick
import Quickshell
import qs.modules.bar

// atrium's desktop shell.
ShellRoot {
    Variants {
        model: Quickshell.screens

        Bar {
            required property ShellScreen modelData

            screen: modelData
        }
    }
}
