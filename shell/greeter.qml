import QtQuick
import Atrium.Shell
import shell.modules.greeter

// atrium's login screen: what `atrium --greeter` shows under greetd.
ShellRoot {
    Variants {
        model: Shell.screens

        GreeterScreen {
            required property ShellScreen modelData

            screen: modelData
        }
    }
}
