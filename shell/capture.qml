//@ pragma AppId atrium-screenshot

import QtQuick
import Atrium.Shell
import Atrium
import shell.modules.capture

// Screenshots, its own process: the screens frozen and covered for picking,
// then the shot in the corner, which opens the editor. The Screenshot portal
// runs it too and reads back the file.
ShellRoot {
    Variants {
        model: Capture.picking ? Shell.screens : []

        CaptureOverlay {
            required property ShellScreen modelData

            screen: modelData
        }
    }

    Thumbnail {
        id: thumbnail

        onOpen: markup.visible = true
    }

    MarkupWindow {
        id: markup
    }
}
