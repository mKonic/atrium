import QtQuick
import QtQuick.Effects
import Atrium.Shell
import shell.components
import shell.services
import Atrium

// Log out, restart or shut down waiting on apps that haven't quit, as
// Windows lists them: answer their own "Save changes?", cancel, or go ahead
// anyway; doing nothing goes ahead when the count runs out. No scrim and no
// keyboard grab: the apps' own questions must stay reachable.
PanelWindow {
    id: root

    readonly property string action: Session.waitingFor
    readonly property var words: ({
            "restart": { verb: "restart", button: qsTr("Restart Anyway"), glyph: "restart_alt" },
            "shutdown": { verb: "shut down", button: qsTr("Shut Down Anyway"), glyph: "power_settings_new" },
            "logout": { verb: "log out", button: qsTr("Log Out Anyway"), glyph: "logout" }
        })
    readonly property var w: words[action] ?? words.shutdown
    readonly property int count: Session.holdouts.length

    visible: action !== ""
    screen: Shell.screen(Atrium.focusedOutput?.name)
    anchors.top: true
    margins.top: 64
    implicitWidth: 420
    implicitHeight: card.height + 32
    exclusiveZone: 0
    color: "transparent"
    WlrLayershell.layer: WlrLayer.Overlay
    WlrLayershell.namespace: "atrium-logout-waiting"

    Rectangle {
        id: card

        anchors.horizontalCenter: parent.horizontalCenter
        y: 8
        width: 380
        height: column.implicitHeight + 48
        radius: 26
        color: Theme.material.thick

        Glass {}

        border.width: Theme.lens ? 0 : 1
        border.color: Theme.palette.separator

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
                text: root.count === 1 ? qsTr("%1 hasn't quit").arg(Session.holdouts[0])
                                       : `${root.count} apps haven't quit`
                font.weight: Font.DemiBold
            }

            StyledText {
                width: parent.width
                visible: root.count > 1
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
                text: Session.holdouts.join(", ")
            }

            StyledText {
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
                text: (root.count === 1 ? qsTr("It may be asking about unsaved work. ") : qsTr("They may be asking about unsaved work. "))
                      + (root.action === "logout" ? `If you do nothing, you will be logged out anyway in ${Session.waitingSeconds} seconds.`
                                                  : `If you do nothing, the computer will ${root.w.verb} anyway in ${Session.waitingSeconds} seconds.`)
                font.pointSize: Theme.font.size.small
                color: Theme.palette.secondaryLabel
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
                    onClicked: Session.stopWaiting()
                }

                DialogButton {
                    width: 168  // "Shut Down Anyway"
                    text: root.w.button
                    primary: true
                    onClicked: Session.goAnyway()
                }
            }
        }
    }
}
