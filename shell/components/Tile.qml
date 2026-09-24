import QtQuick
import shell.services

// A Control Center toggle: icon, title, and a line saying what it is doing.
// As in macOS, the tile stays glass and its icon's circle takes the accent
// when on.
Rectangle {
    id: root

    property string icon: ""
    property string title: ""
    property string subtitle: ""
    property bool on: false
    property bool wide: false        // two-line tile in the top row
    property bool expandable: false  // has a list behind a chevron
    property bool compact: false     // small square tile: icon over the title
    signal toggled
    signal expand

    implicitHeight: compact ? 78 : wide ? 64 : 58
    radius: 18
    color: Theme.palette.tertiaryFill

    component Badge: Rectangle {
        width: 36
        height: 36
        radius: 18
        color: root.on ? Theme.palette.accent : Theme.palette.tertiaryFill

        Behavior on color {
            CAnim {
                duration: Theme.anim.small
            }
        }

        MaterialIcon {
            anchors.centerIn: parent
            text: root.icon
            fill: root.on ? 1 : 0
            font.pointSize: Theme.font.size.larger
            color: Theme.palette.label
        }
    }

    Badge {
        id: badge

        visible: !root.compact
        anchors.left: parent.left
        anchors.leftMargin: 10
        anchors.verticalCenter: parent.verticalCenter
    }

    // Small tile: icon, then the title and state under it.
    Column {
        visible: root.compact
        anchors.centerIn: parent
        width: parent.width - 12
        spacing: 2

        Badge {
            anchors.horizontalCenter: parent.horizontalCenter
        }

        StyledText {
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            text: root.title
            // Shrink a long title to fit rather than cut it.
            fontSizeMode: Text.HorizontalFit
            minimumPointSize: 8
            font.pointSize: Theme.font.size.small
            font.weight: Font.DemiBold
            color: Theme.palette.label
        }

        StyledText {
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            text: root.subtitle
            elide: Text.ElideRight
            font.pointSize: Theme.font.size.small
            color: Theme.palette.secondaryLabel
        }
    }

    Column {
        visible: !root.compact
        anchors.left: badge.right
        anchors.leftMargin: 10
        anchors.right: chevron.visible ? chevron.left : parent.right
        anchors.rightMargin: 8
        anchors.verticalCenter: parent.verticalCenter

        StyledText {
            width: parent.width
            text: root.title
            elide: Text.ElideRight
            font.pointSize: Theme.font.size.smaller
            font.weight: Font.DemiBold
            color: Theme.palette.label
        }

        StyledText {
            width: parent.width
            visible: text.length > 0
            text: root.subtitle
            elide: Text.ElideRight
            font.pointSize: Theme.font.size.small
            color: Theme.palette.secondaryLabel
        }
    }

    MouseArea {
        anchors.fill: parent
        onClicked: root.toggled()
    }

    // The list behind it (networks, devices).
    Rectangle {
        id: chevron

        visible: root.expandable
        anchors.right: parent.right
        anchors.rightMargin: 8
        anchors.verticalCenter: parent.verticalCenter
        width: 28
        height: 28
        radius: 14
        color: chevronArea.containsMouse ? Theme.palette.secondaryFill : "transparent"

        MaterialIcon {
            anchors.centerIn: parent
            text: "chevron_right"
            font.pointSize: Theme.font.size.normal
            color: Theme.palette.secondaryLabel
        }

        MouseArea {
            id: chevronArea

            anchors.fill: parent
            hoverEnabled: true
            onClicked: root.expand()
        }
    }
}
