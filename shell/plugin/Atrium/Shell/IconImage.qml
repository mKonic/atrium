import QtQuick

// An icon at a square size, sharp at any scale: `IconImage { source:
// Shell.iconPath("firefox"); implicitSize: 32 }`.
Item {
    id: root

    property alias source: image.source
    property alias asynchronous: image.asynchronous
    property alias status: image.status
    property alias mipmap: image.mipmap
    property real implicitSize: 0
    // An icon that changes size all the time (the Dock's, magnifying) is
    // drawn once at the largest it gets and scaled, not decoded again at
    // every size: an asynchronous reload leaves it blank for a moment.
    property real largestSize: 0
    readonly property real actualSize: Math.min(width, height)

    implicitWidth: implicitSize
    implicitHeight: implicitSize

    Image {
        id: image

        anchors.fill: parent
        fillMode: Image.PreserveAspectFit
        sourceSize.width: root.largestSize > 0 ? root.largestSize : root.actualSize
        sourceSize.height: root.largestSize > 0 ? root.largestSize : root.actualSize
    }
}
