pragma ComponentBehavior: Bound

import QtQuick
import Atrium
import shell.components
import shell.services

// Software Update, as on a Mac: whether the system is up to date, what's
// newer, and one button that installs it all (with the password, where the
// system wants one).
Column {
    id: root

    spacing: 20

    MissingNote {
        needs: "packagekit"
        explanation: "Updates are found and installed through PackageKit (the packagekit package)."
    }

    // The state, and what to do about it.
    Rectangle {
        visible: !Requirements.missing.packagekit
        width: parent.width
        height: head.implicitHeight + 36
        radius: 14
        color: Theme.palette.m3SurfaceContainer
        border.width: 1
        border.color: Theme.alpha(Theme.palette.m3Outline, 0.12)

        Column {
            id: head

            x: 18
            y: 18
            width: parent.width - 36
            spacing: 12

            Row {
                width: parent.width
                spacing: 14

                Rectangle {
                    width: 48
                    height: 48
                    radius: 14
                    color: Updates.count > 0 ? Theme.palette.m3Primary : Theme.alpha(Theme.palette.m3OnSurface, 0.08)

                    MaterialIcon {
                        anchors.centerIn: parent
                        text: Updates.installing ? "downloading" : Updates.count > 0 ? "system_update_alt" : "check_circle"
                        fill: 1
                        font.pointSize: Theme.font.size.large + 4
                        color: Updates.count > 0 ? Theme.palette.m3OnPrimary : Theme.palette.m3OnSurfaceVariant
                    }
                }

                Column {
                    anchors.verticalCenter: parent.verticalCenter
                    width: parent.width - 62 - buttons.width
                    spacing: 2

                    StyledText {
                        width: parent.width
                        text: Updates.installing ? "Installing updates…"
                            : Updates.checking ? "Checking for updates…"
                            : !Updates.known ? "Looking for updates…"
                            : Updates.count > 0 ? Updates.summary : "Your computer is up to date"
                        font.weight: Font.DemiBold
                        font.pointSize: Theme.font.size.larger
                        elide: Text.ElideRight
                    }

                    StyledText {
                        width: parent.width
                        text: Updates.installing ? (Updates.doing || "Getting ready")
                            : Updates.lastChecked ? `Last checked: ${Updates.lastChecked}` : "Not checked yet"
                        font.pointSize: Theme.font.size.small
                        color: Theme.palette.m3OnSurfaceVariant
                        elide: Text.ElideRight
                    }
                }

                Row {
                    id: buttons

                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 8

                    PillButton {
                        visible: !Updates.installing
                        enabled: !Updates.checking
                        opacity: enabled ? 1 : 0.5
                        text: "Check Now"
                        onClicked: Updates.check()
                    }

                    PillButton {
                        visible: !Updates.installing && Updates.count > 0
                        primary: true
                        text: "Update Now"
                        onClicked: Updates.install()
                    }

                    PillButton {
                        visible: Updates.installing
                        text: "Cancel"
                        onClicked: Updates.cancel()
                    }
                }
            }

            // How far along.
            Rectangle {
                visible: Updates.installing
                width: parent.width
                height: 6
                radius: 3
                color: Theme.alpha(Theme.palette.m3OnSurface, 0.1)

                Rectangle {
                    width: parent.width * Updates.progress / 100
                    height: parent.height
                    radius: 3
                    color: Theme.palette.m3Primary

                    Behavior on width {
                        Anim {}
                    }
                }
            }

            StyledText {
                visible: Updates.error !== ""
                width: parent.width
                wrapMode: Text.WordWrap
                text: Updates.error
                font.pointSize: Theme.font.size.small
                color: "#ffb4ab"
            }

            // Some of what was installed only takes effect after a restart.
            Row {
                visible: Updates.restartNeeded && !Updates.installing
                width: parent.width
                spacing: 12

                StyledText {
                    anchors.verticalCenter: parent.verticalCenter
                    width: parent.width - restart.width - 12
                    wrapMode: Text.WordWrap
                    text: "Restart to finish: the system's core was updated."
                    font.pointSize: Theme.font.size.small
                }

                PillButton {
                    id: restart

                    primary: true
                    text: "Restart…"
                    onClicked: Atrium.action("shell", "session:restart")
                }
            }
        }
    }

    // What's newer.
    Group {
        visible: !Requirements.missing.packagekit && Updates.count > 0
        width: parent.width
        title: "Updates"

        Repeater {
            model: Updates.packages

            ControlRow {
                id: update

                required property var modelData

                title: modelData.name
                note: modelData.summary

                Row {
                    spacing: 8

                    Rectangle {
                        anchors.verticalCenter: parent.verticalCenter
                        visible: update.modelData.security
                        width: securityLabel.implicitWidth + 12
                        height: 20
                        radius: 6
                        color: Theme.alpha("#ff453a", 0.2)

                        StyledText {
                            id: securityLabel

                            anchors.centerIn: parent
                            text: "Security"
                            font.pointSize: Theme.font.size.smaller
                            color: "#ff6b5f"
                        }
                    }

                    StyledText {
                        anchors.verticalCenter: parent.verticalCenter
                        text: update.modelData.version
                        font.pointSize: Theme.font.size.small
                        color: Theme.palette.m3OnSurfaceVariant
                    }
                }
            }
        }
    }
}
