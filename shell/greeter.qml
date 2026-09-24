//@ pragma UseQApplication

import QtQuick
import Quickshell
import qs.modules.greeter

// atrium's login screen: what `atrium --greeter` shows under greetd.
ShellRoot {
    Variants {
        model: Quickshell.screens

        GreeterScreen {
            required property ShellScreen modelData

            screen: modelData
        }
    }
}
