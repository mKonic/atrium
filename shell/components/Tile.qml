import QtQuick
import qs.services

// A Control Center toggle: icon, title, and a line saying what it is doing.
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
    color: on ? Theme.palette.m3Primary : Theme.palette.m3SurfaceContainerHigh

    Behavior on color {
        CAnim {
            duration: Theme.anim.small
        }
    }

    Rectangle {
        id: badge

        visible: !root.compact
        anchors.left: parent.left
        anchors.leftMargin: 10
        anchors.verticalCenter: parent.verticalCenter
        width: 36
        height: 36
        radius: 18
        color: root.on ? Theme.alpha(Theme.palette.m3OnPrimary, 0.14) : Theme.alpha(Theme.palette.m3OnSurface, 0.08)

        MaterialIcon {
            anchors.centerIn: parent
            text: root.icon
            fill: root.on ? 1 : 0
            font.pointSize: Theme.font.size.larger
            color: root.on ? Theme.palette.m3OnPrimary : Theme.palette.m3OnSurface
        }
    }

    // Small tile: icon, then the title and state under it.
    Column {
        visible: root.compact
        anchors.centerIn: parent
        width: parent.width - 12
        spacing: 2

        MaterialIcon {
            anchors.horizontalCenter: parent.horizontalCenter
            text: root.icon
            fill: root.on ? 1 : 0
            font.pointSize: Theme.font.size.larger
            color: root.on ? Theme.palette.m3OnPrimary : Theme.palette.m3OnSurface
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
            color: root.on ? Theme.palette.m3OnPrimary : Theme.palette.m3OnSurface
        }

        StyledText {
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            text: root.subtitle
            elide: Text.ElideRight
            font.pointSize: Theme.font.size.small
            color: root.on ? Theme.alpha(Theme.palette.m3OnPrimary, 0.8) : Theme.palette.m3OnSurfaceVariant
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
            color: root.on ? Theme.palette.m3OnPrimary : Theme.palette.m3OnSurface
        }

        StyledText {
            width: parent.width
            visible: text.length > 0
            text: root.subtitle
            elide: Text.ElideRight
            font.pointSize: Theme.font.size.small
            color: root.on ? Theme.alpha(Theme.palette.m3OnPrimary, 0.8) : Theme.palette.m3OnSurfaceVariant
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
        color: chevronArea.containsMouse ? Theme.alpha(root.on ? Theme.palette.m3OnPrimary : Theme.palette.m3OnSurface, 0.12) : "transparent"

        MaterialIcon {
            anchors.centerIn: parent
            text: "chevron_right"
            font.pointSize: Theme.font.size.normal
            color: root.on ? Theme.palette.m3OnPrimary : Theme.palette.m3OnSurfaceVariant
        }

        MouseArea {
            id: chevronArea

            anchors.fill: parent
            hoverEnabled: true
            onClicked: root.expand()
        }
    }
}
