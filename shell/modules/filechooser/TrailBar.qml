pragma ComponentBehavior: Bound

import QtQuick
import shell.components
import shell.services
import Atrium

// GTK's path bar: a button for each folder down to where the list is, the
// current one pressed. Going up keeps the deeper ones, so a click goes
// back down. Inside home it starts at Home; the folders above are behind
// the arrow at its start (GTK's fake root). Long trails scroll. A click
// beside the buttons asks to type the location instead.
Item {
    id: root

    required property ChooserModel model
    signal editRequested
    property bool showAbove: false
    readonly property var shown: model.trail.filter(c => showAbove || !c.aboveHome)
    readonly property bool hasAbove: model.trail.some(c => c.aboveHome)

    implicitHeight: 32

    // The room beside the buttons: looks like a field under the pointer,
    // and a click (or tap) there turns the bar into one.
    Rectangle {
        anchors.fill: parent
        radius: 8
        color: editArea.containsMouse ? Theme.palette.quaternaryFill : "transparent"

        MouseArea {
            id: editArea

            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.IBeamCursor
            onClicked: root.editRequested()
        }
    }

    // The ones above home.
    Rectangle {
        id: above

        visible: root.hasAbove
        width: visible ? 26 : 0
        height: 28
        anchors.verticalCenter: parent.verticalCenter
        radius: 6
        color: aboveArea.containsMouse ? Theme.palette.secondaryFill : Theme.palette.tertiaryFill

        MaterialIcon {
            anchors.centerIn: parent
            text: root.showAbove ? "chevron_right" : "chevron_left"
            color: Theme.palette.secondaryLabel
        }

        MouseArea {
            id: aboveArea

            anchors.fill: parent
            hoverEnabled: true
            onClicked: root.showAbove = !root.showAbove
        }
    }

    Flickable {
        id: flick

        anchors.left: above.right
        anchors.leftMargin: above.visible ? 4 : 0
        // Only as wide as the buttons, so the room after them is free to click.
        width: Math.min(root.width - x, buttons.width)
        height: parent.height
        clip: true
        contentWidth: buttons.width
        boundsBehavior: Flickable.StopAtBounds
        flickableDirection: Flickable.HorizontalFlick

        // The current one stays in view.
        function reveal(): void {
            const b = buttons.children.find(c => c.current === true);
            if (!b)
                return;
            if (b.x < contentX)
                contentX = b.x;
            else if (b.x + b.width > contentX + width)
                contentX = b.x + b.width - width;
        }

        Row {
            id: buttons

            height: parent.height
            spacing: 2
            onWidthChanged: Qt.callLater(flick.reveal)

            Repeater {
                model: root.shown

                Rectangle {
                    id: crumb

                    required property var modelData
                    readonly property bool current: modelData.current

                    anchors.verticalCenter: parent.verticalCenter
                    width: content.implicitWidth + 16
                    height: 28
                    radius: 6
                    color: current ? Theme.palette.secondaryFill : crumbArea.containsMouse ? Theme.palette.tertiaryFill : "transparent"
                    border.width: current ? 1 : 0
                    border.color: Theme.palette.separator

                    Accessible.role: Accessible.Button
                    Accessible.name: modelData.kind === "root" ? qsTr("File System Root") : modelData.name

                    Row {
                        id: content

                        anchors.centerIn: parent
                        spacing: 4

                        MaterialIcon {
                            visible: crumb.modelData.kind !== "folder"
                            anchors.verticalCenter: parent.verticalCenter
                            text: crumb.modelData.kind === "root" ? "hard_drive" : crumb.modelData.kind === "home" ? "home" : "desktop_windows"
                            color: crumb.current ? Theme.palette.label : Theme.palette.secondaryLabel
                        }

                        StyledText {
                            visible: text !== ""
                            anchors.verticalCenter: parent.verticalCenter
                            text: crumb.modelData.name
                            font.pointSize: Theme.font.size.small
                            font.weight: crumb.current ? Font.DemiBold : Font.Normal
                            color: crumb.current ? Theme.palette.label : Theme.palette.secondaryLabel
                        }
                    }

                    MouseArea {
                        id: crumbArea

                        anchors.fill: parent
                        hoverEnabled: true
                        onClicked: root.model.goTrail(crumb.modelData.path)
                    }
                }
            }
        }
    }
}
