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
            "restart": { verb: "restart", button: qsTr("Restart"), glyph: "restart_alt" },
            "shutdown": { verb: "shut down", button: qsTr("Shut Down"), glyph: "power_settings_new" },
            "logout": { verb: "log out", button: qsTr("Log Out"), glyph: "logout" }
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
    color: Theme.palette.scrim
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
        color: Theme.material.thick

        Glass {}

        border.width: Theme.lens ? 0 : 1
        border.color: Theme.palette.separator
        focus: true
        Keys.onEscapePressed: Session.cancel()
        Keys.onReturnPressed: Session.confirm()
        Keys.onEnterPressed: Session.confirm()

        layer.enabled: true
        layer.effect: MultiEffect {
            shadowEnabled: !Theme.lens
            shadowColor: Theme.palette.shadow
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
                color: Theme.palette.accent

                MaterialIcon {
                    anchors.centerIn: parent
                    text: root.w.glyph
                    font.pointSize: 22
                    color: Theme.palette.labelOnAccent
                }
            }

            StyledText {
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
                text: root.action === "logout" ? qsTr("Are you sure you want to quit all apps and log out now?")
                                               : `Are you sure you want to ${root.w.verb} your computer now?`
                font.weight: Font.DemiBold
            }

            StyledText {
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
                text: root.action === "logout" ? qsTr("If you do nothing, you will be logged out automatically in %1 seconds.").arg(Session.secondsLeft)
                                               : `If you do nothing, the computer will ${root.w.verb} automatically in ${Session.secondsLeft} seconds.`
                font.pointSize: Theme.font.size.small
                color: Theme.palette.secondaryLabel
            }

            // macOS's checkbox: the apps open now open again at the next login.
            Row {
                anchors.horizontalCenter: parent.horizontalCenter
                spacing: 8

                Switch {
                    name: qsTr("Reopen windows when logging back in")
                    anchors.verticalCenter: parent.verticalCenter
                    checked: Atrium.settings["session.reopen_windows"] ?? true
                    onToggled: Atrium.setSetting("session.reopen_windows", !checked)
                }

                StyledText {
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("Reopen windows when logging back in")
                    font.pointSize: Theme.font.size.small
                }
            }

            Item {
                width: 1
                height: 6
            }

            Row {
                anchors.horizontalCenter: parent.horizontalCenter
                spacing: 10

                DialogButton {
                    text: qsTr("Cancel")
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
}
