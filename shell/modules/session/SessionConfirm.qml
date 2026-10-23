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
        width: 350
        height: column.implicitHeight + 42
        radius: 35
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

        // The kit's alert, side by side: its symbol, the question in bold
        // and what will happen, left-aligned, over two buttons that share
        // the width.
        Column {
            id: column

            anchors.horizontalCenter: parent.horizontalCenter
            y: 21
            width: parent.width - 42
            spacing: 10

            Rectangle {
                width: 48
                height: 48
                radius: 24
                color: Theme.palette.accent

                MaterialIcon {
                    anchors.centerIn: parent
                    text: root.w.glyph
                    font.pointSize: 19
                    color: Theme.palette.labelOnAccent
                }
            }

            StyledText {
                width: parent.width
                wrapMode: Text.WordWrap
                text: root.action === "logout" ? qsTr("Are you sure you want to quit all apps and log out now?")
                                               : `Are you sure you want to ${root.w.verb} your computer now?`
                font.weight: Font.Bold
            }

            StyledText {
                width: parent.width
                wrapMode: Text.WordWrap
                text: root.action === "logout" ? qsTr("If you do nothing, you will be logged out automatically in %1 seconds.").arg(Session.secondsLeft)
                                               : `If you do nothing, the computer will ${root.w.verb} automatically in ${Session.secondsLeft} seconds.`
            }

            // macOS's checkbox: the apps open now open again at the next login.
            Row {
                width: parent.width
                spacing: 8

                Switch {
                    id: reopen

                    name: qsTr("Reopen windows when logging back in")
                    anchors.verticalCenter: parent.verticalCenter
                    checked: Atrium.settings["session.reopen_windows"] ?? true
                    onToggled: Atrium.setSetting("session.reopen_windows", !checked)
                }

                StyledText {
                    anchors.verticalCenter: parent.verticalCenter
                    width: parent.width - reopen.width - parent.spacing
                    wrapMode: Text.WordWrap
                    text: qsTr("Reopen windows when logging back in")
                    color: Theme.palette.secondaryLabel
                }
            }

            Item {
                width: 1
                height: 6
            }

            Row {
                spacing: 10

                DialogButton {
                    width: (column.width - 10) / 2
                    text: qsTr("Cancel")
                    onClicked: Session.cancel()
                }

                DialogButton {
                    width: (column.width - 10) / 2
                    text: root.w.button
                    primary: true
                    onClicked: Session.confirm()
                }
            }
        }
    }
}
