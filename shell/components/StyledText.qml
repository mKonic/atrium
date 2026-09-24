import QtQuick
import shell.services

Text {
    color: Theme.palette.label
    font.family: Theme.font.sans
    font.pointSize: Theme.font.size.normal
    renderType: Text.NativeRendering
    verticalAlignment: Text.AlignVCenter

    Behavior on color {
        CAnim {}
    }
}
