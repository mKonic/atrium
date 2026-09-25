pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import shell.components
import shell.services

// Pick one of many: a button showing the choice, a list under it.
// `options` is [{ value, label }].
Rectangle {
    id: root

    property var options: []
    property string value
    property int fieldWidth: 200
    property string placeholder: "Choose…"  // while nothing is picked
    signal picked(string value)

    readonly property string label: options.find(o => o.value === value)?.label ?? (value || placeholder)

    implicitWidth: fieldWidth
    implicitHeight: 30
    radius: 8
    color: area.containsMouse || popup.visible ? Theme.palette.secondaryFill : Theme.palette.tertiaryFill

    StyledText {
        anchors.left: parent.left
        anchors.leftMargin: 10
        anchors.right: chevron.left
        anchors.verticalCenter: parent.verticalCenter
        text: root.label
        color: root.value ? Theme.palette.label : Theme.palette.secondaryLabel
        elide: Text.ElideRight
        font.pointSize: Theme.font.size.small
    }

    MaterialIcon {
        id: chevron

        anchors.right: parent.right
        anchors.rightMargin: 6
        anchors.verticalCenter: parent.verticalCenter
        text: "unfold_more"
        font.pointSize: Theme.font.size.normal
        color: Theme.palette.secondaryLabel
    }

    MouseArea {
        id: area

        anchors.fill: parent
        hoverEnabled: true
        onClicked: popup.open()
    }

    Popup {
        id: popup

        y: root.height + 4
        width: Math.max(root.width, 180)
        height: Math.min(list.contentHeight + 12, 320)
        padding: 6
        // Never past the window's edges (Qt clamps it inside them)...
        margins: 8
        // ...and above the field when there's no room under it, as a Mac's do.
        onAboutToShow: {
            const top = root.mapToItem(null, 0, 0).y;
            const below = (root.Window.height ?? 0) - top - root.height - 12;
            y = below < height && top - 12 >= height ? -height - 4 : root.height + 4;
        }
        // Long lists open at the current choice.
        onOpened: list.positionViewAtIndex(root.options.findIndex(o => o.value === root.value), ListView.Center)
        background: Rectangle {
            radius: 12
            color: Theme.palette.windowBackground
            border.width: 1
            border.color: Theme.palette.separator
        }

        ListView {
            id: list

            anchors.fill: parent
            clip: true
            model: root.options
            boundsBehavior: Flickable.StopAtBounds

            delegate: Rectangle {
                id: option

                required property var modelData

                width: list.width
                height: 30
                radius: 7
                color: optionArea.containsMouse ? Theme.palette.tertiaryFill : "transparent"

                MaterialIcon {
                    id: tick

                    x: 6
                    anchors.verticalCenter: parent.verticalCenter
                    text: "check"
                    visible: option.modelData.value === root.value
                    font.pointSize: Theme.font.size.small
                    color: Theme.palette.accent
                }

                StyledText {
                    x: 28
                    anchors.verticalCenter: parent.verticalCenter
                    text: option.modelData.label
                    font.pointSize: Theme.font.size.small
                }

                MouseArea {
                    id: optionArea

                    anchors.fill: parent
                    hoverEnabled: true
                    onClicked: {
                        popup.close();
                        if (option.modelData.value !== root.value)
                            root.picked(option.modelData.value);
                    }
                }
            }
        }
    }
}
