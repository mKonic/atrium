import QtQuick
import shell.services

// A Material Symbols glyph by name ("terminal", "chat").
StyledText {
    property real fill: 0

    // A glyph's text is its ligature name ("settings"): what it's for is the
    // control's to say, not the icon's.
    Accessible.ignored: true

    font.family: Theme.font.icons
    font.pointSize: Theme.font.size.larger
    font.variableAxes: ({
        FILL: fill,
        opsz: fontInfo.pixelSize,
        wght: fontInfo.weight
    })
}
