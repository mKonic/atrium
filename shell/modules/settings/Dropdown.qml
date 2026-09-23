pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import qs.components
import qs.services

// Pick one of many: a button showing the choice, a list under it.
// `options` is [{ value, label }].
Rectangle {
    id: root

    property var options: []
    property string value
    property int fieldWidth: 200
    signal picked(string value)

    readonly property string label: options.find(o => o.value === value)?.label ?? value

    implicitWidth: fieldWidth
    implicitHeight: 30
    radius: 8
    color: area.containsMouse || popup.visible ? Theme.alpha(Theme.palette.m3OnSurface, 0.12) : Theme.alpha(Theme.palette.m3OnSurface, 0.07)

    StyledText {
        anchors.left: parent.left
        anchors.leftMargin: 10
        anchors.right: chevron.left
        anchors.verticalCenter: parent.verticalCenter
        text: root.label
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
        color: Theme.palette.m3OnSurfaceVariant
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
        background: Rectangle {
            radius: 12
            color: Theme.palette.m3SurfaceContainerHigh
            border.width: 1
            border.color: Theme.alpha(Theme.palette.m3OutlineVariant, 0.6)
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
                color: optionArea.containsMouse ? Theme.alpha(Theme.palette.m3OnSurface, 0.08) : "transparent"

                MaterialIcon {
                    id: tick

                    x: 6
                    anchors.verticalCenter: parent.verticalCenter
                    text: "check"
                    visible: option.modelData.value === root.value
                    font.pointSize: Theme.font.size.small
                    color: Theme.palette.m3Primary
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
