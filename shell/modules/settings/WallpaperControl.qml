import QtCore
import QtQuick
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
        color: Atrium.settings["appearance.background"] ?? Theme.palette.tertiaryFill
        border.width: 1
        border.color: Theme.palette.separator
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
        text: qsTr("Choose…")
        onClicked: picker.open()
    }

    FilePicker {
        id: picker

        title: qsTr("Choose a Wallpaper")
        folder: StandardPaths.writableLocation(StandardPaths.PicturesLocation)
        nameFilter: qsTr("Pictures (*.png *.jpg *.jpeg *.webp *.avif *.jxl *.bmp)")
        onPicked: file => root.picked(file)
    }
}
