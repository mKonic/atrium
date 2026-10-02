//@ pragma AppId atrium-lock
import QtQuick
import Atrium.Shell
import shell.modules.greeter

// The lock screen: started by atrium (Super+L, logind's lock) with the
// atrium-lock shell integration, so its windows are lock surfaces.
ShellRoot {
    Variants {
        model: Shell.screens

        LockScreen {
            required property ShellScreen modelData

            screen: modelData
        }
    }
}
