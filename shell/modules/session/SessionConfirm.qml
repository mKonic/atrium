import QtQuick
import QtQuick.Effects
import Atrium.Shell
import shell.components
import shell.services
import Atrium

// "Are you sure you want to shut down your computer now?" with the minute
// counting down, as macOS asks.
PanelWindow {
    id: root

    readonly property string action: Session.pending
    readonly property var words: ({
            "restart": { verb: "restart", button: "Restart", glyph: "restart_alt" },
            "shutdown": { verb: "shut down", button: "Shut Down", glyph: "power_settings_new" },
            "logout": { verb: "log out", button: "Log Out", glyph: "logout" }
        })
    readonly property var w: words[action] ?? words.shutdown

    visible: action !== ""
    screen: Shell.screen(Atrium.focusedOutput?.name)
    anchors {
        top: true
        bottom: true
        left: true
        right: true
    }
    exclusiveZone: -1
    color: Qt.rgba(0, 0, 0, 0.35)
    WlrLayershell.layer: WlrLayer.Overlay
    WlrLayershell.namespace: "atrium-session-confirm"
    WlrLayershell.keyboardFocus: visible ? WlrKeyboardFocus.Exclusive : WlrKeyboardFocus.None

    onVisibleChanged: if (visible) card.forceActiveFocus()

    MouseArea {
        anchors.fill: parent  // the desktop waits
    }

    Rectangle {
        id: card

        anchors.centerIn: parent
        width: 340
        height: column.implicitHeight + 48
        radius: 26
        color: Theme.panel(Theme.palette.m3SurfaceContainer, 0.9)

        GlassRim {}

        border.width: 1
        border.color: Theme.alpha(Theme.palette.m3Outline, 0.22)
        focus: true
        Keys.onEscapePressed: Session.cancel()
        Keys.onReturnPressed: Session.confirm()
        Keys.onEnterPressed: Session.confirm()

        layer.enabled: true
        layer.effect: MultiEffect {
            shadowEnabled: true
            shadowColor: Qt.rgba(0, 0, 0, 0.5)
            shadowBlur: 1
            shadowVerticalOffset: 8
        }

        Column {
            id: column

            anchors.horizontalCenter: parent.horizontalCenter
            y: 24
            width: parent.width - 48
            spacing: 10

            Rectangle {
                anchors.horizontalCenter: parent.horizontalCenter
                width: 56
                height: 56
                radius: 28
                color: Theme.palette.m3Primary

                MaterialIcon {
                    anchors.centerIn: parent
                    text: root.w.glyph
                    font.pointSize: 22
                    color: Theme.palette.m3OnPrimary
                }
            }

            StyledText {
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
                text: root.action === "logout" ? "Are you sure you want to quit all apps and log out now?"
                                               : `Are you sure you want to ${root.w.verb} your computer now?`
                font.weight: Font.DemiBold
            }

            StyledText {
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
                text: root.action === "logout" ? `If you do nothing, you will be logged out automatically in ${Session.secondsLeft} seconds.`
                                               : `If you do nothing, the computer will ${root.w.verb} automatically in ${Session.secondsLeft} seconds.`
                font.pointSize: Theme.font.size.small
                color: Theme.palette.m3OnSurfaceVariant
            }

            Item {
                width: 1
                height: 6
            }

            Row {
                anchors.horizontalCenter: parent.horizontalCenter
                spacing: 10

                DialogButton {
                    text: "Cancel"
                    onClicked: Session.cancel()
                }

                DialogButton {
                    text: root.w.button
                    primary: true
                    onClicked: Session.confirm()
                }
            }
        }
    }

    component DialogButton: Rectangle {
        id: button

        property string text
        property bool primary: false
        signal clicked

        width: 136
        height: 36
        radius: 18
        color: primary ? (area.containsMouse ? Qt.lighter(Theme.palette.m3Primary, 1.08) : Theme.palette.m3Primary)
                       : (area.containsMouse ? Theme.alpha(Theme.palette.m3OnSurface, 0.16) : Theme.alpha(Theme.palette.m3OnSurface, 0.1))

        StyledText {
            anchors.centerIn: parent
            text: button.text
            font.weight: Font.DemiBold
            color: button.primary ? Theme.palette.m3OnPrimary : Theme.palette.m3OnSurface
        }

        MouseArea {
            id: area

            anchors.fill: parent
            hoverEnabled: true
            onClicked: button.clicked()
        }
    }
}
