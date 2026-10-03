import QtQuick
import Atrium.Shell
import shell.modules.greeter
import shell.modules.controlcenter

// atrium's login screen: what `atrium --greeter` shows under atrium-login or greetd.
ShellRoot {
    Variants {
        model: Shell.screens

        GreeterScreen {
            required property ShellScreen modelData

            screen: modelData
        }
    }

    ControlCenter {
        greeter: true
    }
}
