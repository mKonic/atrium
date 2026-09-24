import QtQuick
import Atrium.Shell
import Atrium

// The wallpaper on one screen, under everything (the desktop colour shows
// where a fitted picture leaves room). A new picture replaces the old one
// once it has loaded, never through a blank.
PanelWindow {
    id: root

    readonly property url source: Wallpaper.url(Atrium.settings["appearance.wallpaper"] ?? "")
    readonly property string fit: Atrium.settings["appearance.wallpaper_fit"] ?? "fill"
    readonly property var fillModes: ({
        fill: Image.PreserveAspectCrop,
        fit: Image.PreserveAspectFit,
        stretch: Image.Stretch,
        center: Image.Pad,
        tile: Image.Tile
    })

    anchors {
        top: true
        bottom: true
        left: true
        right: true
    }
    color: "transparent"
    exclusionMode: ExclusionMode.Ignore
    visible: source != ""
    mask: Region {}
    WlrLayershell.layer: WlrLayer.Background
    WlrLayershell.namespace: "atrium-wallpaper"
    WlrLayershell.keyboardFocus: WlrKeyboardFocus.None

    Image {
        anchors.fill: parent
        source: root.source
        fillMode: root.fillModes[root.fit] ?? Image.PreserveAspectCrop
        // Decoded at the screen's size, not the file's (an 8K picture is 130 MB).
        sourceSize: root.fit === "fill" || root.fit === "fit"
                    ? Qt.size(root.width * root.screen.devicePixelRatio, root.height * root.screen.devicePixelRatio)
                    : undefined
        asynchronous: true
        retainWhileLoading: true
        smooth: true
        mipmap: true
    }
}
