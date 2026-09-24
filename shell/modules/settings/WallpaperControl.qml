import QtCore
import QtQuick
import QtQuick.Dialogs
import shell.components
import shell.services
import Atrium

// The wallpaper: a small preview of the picture (the desktop colour when
// there's none) and a button to choose another.
Row {
    id: root

    property string value
    signal picked(url file)

    spacing: 12

    Rectangle {
        anchors.verticalCenter: parent.verticalCenter
        width: 80
        height: 50
        radius: 8
        color: Atrium.settings["appearance.background"] ?? Theme.palette.m3SurfaceContainerHigh
        border.width: 1
        border.color: Theme.alpha(Theme.palette.m3Outline, 0.2)
        clip: true

        Image {
            anchors.fill: parent
            anchors.margins: 1
            source: Wallpaper.url(root.value)
            sourceSize: Qt.size(160, 100)
            fillMode: Image.PreserveAspectCrop
            asynchronous: true
            layer.enabled: true
        }
    }

    PillButton {
        anchors.verticalCenter: parent.verticalCenter
        text: "Choose…"
        onClicked: picker.open()
    }

    FileDialog {
        id: picker

        title: "Choose a wallpaper"
        currentFolder: StandardPaths.writableLocation(StandardPaths.PicturesLocation)
        nameFilters: ["Pictures (*.png *.jpg *.jpeg *.webp *.avif *.jxl *.bmp)"]
        onAccepted: root.picked(selectedFile)
    }
}
