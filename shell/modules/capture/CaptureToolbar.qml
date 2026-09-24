pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Effects
import shell.components
import shell.services
import Atrium

// Print's bar, as macOS's Shift-Command-5: take the whole screen, a window
// or a selection; record the screen; or close.
Rectangle {
    id: root

    width: row.implicitWidth + 16
    height: 52
    radius: 16
    color: Theme.panel(Theme.palette.m3SurfaceContainer, 0.94)
    border.width: 1
    border.color: Theme.alpha(Theme.palette.m3Outline, 0.22)

    GlassRim {}

    layer.enabled: true
    layer.effect: MultiEffect {
        shadowEnabled: true
        shadowColor: Qt.rgba(0, 0, 0, 0.5)
        shadowBlur: 1
        shadowVerticalOffset: 6
    }

    // Swallow clicks: they aren't a pick.
    MouseArea {
        anchors.fill: parent
    }

    Row {
        id: row

        anchors.centerIn: parent
        spacing: 4

        ToolButton {
            icon: "close"
            tip: "Close"
            onClicked: Capture.cancel()
        }

        Rectangle {
            anchors.verticalCenter: parent.verticalCenter
            width: 1
            height: 26
            color: Theme.alpha(Theme.palette.m3Outline, 0.3)
        }

        ToolButton {
            icon: "desktop_windows"
            tip: "Entire screen"
            chosen: Capture.kind === "screen"
            onClicked: Capture.kind = "screen"
        }

        ToolButton {
            icon: "web_asset"
            tip: "Window"
            chosen: Capture.kind === "window"
            onClicked: Capture.kind = "window"
        }

        ToolButton {
            icon: "select"
            tip: "Selection"
            chosen: Capture.kind === "region"
            onClicked: Capture.kind = "region"
        }

        Rectangle {
            anchors.verticalCenter: parent.verticalCenter
            visible: !Capture.forPortal
            width: 1
            height: 26
            color: Theme.alpha(Theme.palette.m3Outline, 0.3)
        }

        ToolButton {
            visible: !Capture.forPortal && Recorder.available
            icon: "screen_record"
            tip: "Record the screen"
            onClicked: Capture.record()
        }
    }

    // Which one's picked, and what it's called under it.
    StyledText {
        anchors.top: parent.bottom
        anchors.topMargin: 8
        anchors.horizontalCenter: parent.horizontalCenter
        text: Capture.kind === "screen" ? "Click a screen to take it"
            : Capture.kind === "window" ? "Click a window to take it"
            : "Drag across what to take"
        color: "white"
        style: Text.Outline
        styleColor: Qt.rgba(0, 0, 0, 0.5)
        font.pointSize: Theme.font.size.small
    }

    component ToolButton: Rectangle {
        id: button

        property string icon
        property string tip
        property bool chosen: false
        signal clicked

        anchors.verticalCenter: parent?.verticalCenter
        width: 40
        height: 40
        radius: 10
        color: chosen ? Theme.alpha(Theme.palette.m3Primary, 0.28)
             : area.containsMouse ? Theme.alpha(Theme.palette.m3OnSurface, 0.1) : "transparent"

        MaterialIcon {
            anchors.centerIn: parent
            text: button.icon
            font.pointSize: Theme.font.size.large
            color: button.chosen ? Theme.palette.m3Primary : Theme.palette.m3OnSurface
        }

        MouseArea {
            id: area

            anchors.fill: parent
            hoverEnabled: true
            onClicked: button.clicked()
        }
    }
}
