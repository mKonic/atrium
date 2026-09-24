pragma ComponentBehavior: Bound

import QtQuick
import Quickshell
import Quickshell.Widgets
import qs.components
import qs.services
import Atrium

// One notification: the app's icon (or the picture it sent), who and when,
// the title and text, and its buttons.
Rectangle {
    id: root

    property string app: ""
    property string icon: ""       // resolved icon path
    property string image: ""      // a picture that came with it
    property string summary: ""
    property string body: ""
    property real time: 0
    property bool critical: false
    property var actions: []       // [{ text, invoke }]
    property bool compact: false   // in the notification center
    property bool expanded: false  // full text and buttons; drag down or the chevron
    property bool swipeable: true  // swipe sideways to dismiss

    signal dismissed
    signal clicked

    readonly property bool hovered: hover.hovered || swipe.active
    readonly property bool expandable: actions.length > 0 || bodyText.truncated || expanded

    Behavior on implicitHeight {
        Anim {
            duration: Theme.anim.small
        }
    }

    // Sideways: dismiss past a third of the way; up/down: collapse/expand.
    DragHandler {
        id: swipe

        enabled: root.swipeable
        yAxis.enabled: false
        target: root
        onActiveChanged: {
            if (active)
                return;
            if (Math.abs(root.x) > root.width * 0.3)
                root.dismissed();
            else
                back.start();
        }
    }

    DragHandler {
        target: null
        xAxis.enabled: false
        onTranslationChanged: {
            if (Math.abs(translation.y) > 20 && root.expandable)
                root.expanded = translation.y > 0;
        }
    }

    Anim {
        id: back

        target: root
        property: "x"
        to: 0
        duration: Theme.anim.small
    }

    TapHandler {
        acceptedButtons: Qt.MiddleButton
        onTapped: root.dismissed()
    }

    implicitHeight: content.implicitHeight + 24
    radius: 18
    color: Theme.panel(Theme.palette.m3SurfaceContainer, 0.82)

    GlassRim {}

    border.width: critical ? 2 : 1
    border.color: critical ? "#ffb4ab" : Theme.alpha(Theme.palette.m3Outline, 0.2)

    HoverHandler {
        id: hover
    }

    TapHandler {
        onTapped: root.clicked()
    }

    Row {
        id: content

        x: 12
        y: 12
        width: parent.width - 24
        spacing: 12

        Item {
            width: 40
            height: 40

            Rectangle {
                anchors.fill: parent
                radius: 12
                visible: root.image.length > 0
                color: "transparent"
                clip: true

                Image {
                    anchors.fill: parent
                    source: root.image
                    fillMode: Image.PreserveAspectCrop
                    asynchronous: true
                    sourceSize.width: 80
                    sourceSize.height: 80
                }
            }

            IconImage {
                anchors.fill: parent
                visible: root.image.length === 0
                source: root.icon
                // Not async: Quickshell's icon provider crashes when a
                // loader thread is the first to use it (a saved history
                // loading at startup). Icons are small; this costs nothing.
                asynchronous: false
            }
        }

        Column {
            width: parent.width - 52
            spacing: 2

            Row {
                width: parent.width
                spacing: 6

                StyledText {
                    visible: !root.compact  // its group already says
                    text: root.app
                    font.pointSize: Theme.font.size.small
                    font.weight: Font.Medium
                    color: Theme.palette.m3OnSurfaceVariant
                    elide: Text.ElideRight
                    width: Math.min(implicitWidth, parent.width - when.implicitWidth - 30)
                }

                StyledText {
                    id: when

                    text: root.compact ? NotificationHistory.ago(root.time) : `· ${NotificationHistory.ago(root.time)}`
                    font.pointSize: Theme.font.size.small
                    color: Theme.alpha(Theme.palette.m3OnSurfaceVariant, 0.7)
                }
            }

            StyledText {
                width: parent.width
                text: root.summary
                font.weight: Font.DemiBold
                elide: Text.ElideRight
                visible: text.length > 0
            }

            StyledText {
                id: bodyText

                width: parent.width
                text: root.body
                textFormat: Text.StyledText
                wrapMode: Text.Wrap
                maximumLineCount: root.expanded ? 14 : 1
                elide: Text.ElideRight
                color: Theme.palette.m3OnSurfaceVariant
                font.pointSize: Theme.font.size.smaller
                visible: text.length > 0
            }

            Flow {
                width: parent.width
                spacing: 6
                topPadding: 6
                visible: root.expanded && root.actions.length > 0

                Repeater {
                    model: root.actions

                    Rectangle {
                        id: button

                        required property var modelData

                        width: label.implicitWidth + 24
                        height: 30
                        radius: 15
                        color: press.containsMouse ? Theme.alpha(Theme.palette.m3Primary, 0.3) : Theme.alpha(Theme.palette.m3Primary, 0.16)

                        StyledText {
                            id: label

                            anchors.centerIn: parent
                            text: button.modelData.text
                            font.pointSize: Theme.font.size.smaller
                            font.weight: Font.Medium
                            color: Theme.palette.m3Primary
                        }

                        MouseArea {
                            id: press

                            anchors.fill: parent
                            hoverEnabled: true
                            onClicked: button.modelData.invoke()
                        }
                    }
                }
            }
        }
    }

    // More: the whole text and the buttons.
    Rectangle {
        anchors.top: parent.top
        anchors.right: closeButton.left
        anchors.topMargin: 8
        anchors.rightMargin: 4
        width: 22
        height: 22
        radius: 11
        visible: root.expandable
        color: moreArea.containsMouse ? Theme.palette.m3SurfaceContainerHigh : "transparent"

        MaterialIcon {
            anchors.centerIn: parent
            text: root.expanded ? "expand_less" : "expand_more"
            font.pointSize: Theme.font.size.normal
            color: Theme.palette.m3OnSurfaceVariant
        }

        // A MouseArea takes the click, so the card's own tap doesn't fire too.
        MouseArea {
            id: moreArea

            anchors.fill: parent
            hoverEnabled: true
            onClicked: root.expanded = !root.expanded
        }
    }

    // Close, on hover.
    Rectangle {
        id: closeButton

        anchors.top: parent.top
        anchors.right: parent.right
        anchors.margins: 8
        width: 22
        height: 22
        radius: 11
        color: closeArea.containsMouse ? Theme.palette.m3SurfaceContainerHigh : Theme.alpha(Theme.palette.m3SurfaceContainerHigh, 0.8)
        opacity: root.hovered ? 1 : 0
        visible: opacity > 0

        Behavior on opacity {
            Anim {
                duration: Theme.anim.small
            }
        }

        MaterialIcon {
            anchors.centerIn: parent
            text: "close"
            font.pointSize: Theme.font.size.small
        }

        MouseArea {
            id: closeArea

            anchors.fill: parent
            hoverEnabled: true
            onClicked: root.dismissed()
        }
    }
}
