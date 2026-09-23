import QtQuick
import qs.services

// A Material Symbols glyph by name ("terminal", "chat").
StyledText {
    property real fill: 0

    font.family: Theme.font.icons
    font.pointSize: Theme.font.size.larger
    font.variableAxes: ({
        FILL: fill,
        opsz: fontInfo.pixelSize,
        wght: fontInfo.weight
    })
}
