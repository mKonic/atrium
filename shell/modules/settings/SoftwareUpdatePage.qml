pragma ComponentBehavior: Bound

import QtQuick
import Atrium
import shell.components
import shell.services

// Software Update: atrium itself (its GitHub releases, installed for the
// next login), the system (PackageKit, or pacman itself without it) and the
// AUR (through paru or yay). Each says whether it's up to date and has one
// button that updates it.
Column {
    id: root

    // The system through PackageKit when it's installed, else pacman itself.
    readonly property var system: Updates.available ? Updates : PacmanUpdates

    spacing: 20

    // A source's state and what to do about it.
    component SourceCard: Rectangle {
        id: card

        property var source
        property string name
        property string icon
        property string idle: "Up to date"

        width: parent?.width ?? 0
        height: head.implicitHeight + 36
        radius: 14
        color: Theme.palette.groupedBackground
        border.width: 1
        border.color: Theme.palette.separator

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
                    width: 44
                    height: 44
                    radius: 12
                    color: card.source.count > 0 ? Theme.palette.accent : Theme.palette.tertiaryFill

                    MaterialIcon {
                        anchors.centerIn: parent
                        text: card.source.installing ? "downloading" : card.icon
                        fill: 1
                        font.pointSize: Theme.font.size.large + 2
                        color: card.source.count > 0 ? Theme.palette.labelOnAccent : Theme.palette.secondaryLabel
                    }
                }

                Column {
                    anchors.verticalCenter: parent.verticalCenter
                    width: parent.width - 58 - buttons.width
                    spacing: 2

                    StyledText {
                        width: parent.width
                        text: card.name + ": " + (card.source.installing ? qsTr("Updating…")
                            : card.source.checking ? "Checking…"
                            : !card.source.known ? "Not checked yet"
                            : card.source.count > 0 ? card.source.summary : card.idle)
                        font.weight: Font.DemiBold
                        font.pointSize: Theme.font.size.larger
                        elide: Text.ElideRight
                    }

                    StyledText {
                        width: parent.width
                        text: card.source.installing ? (card.source.doing || qsTr("Getting ready"))
                            : card.source.lastChecked ? `Last checked: ${card.source.lastChecked}` : ""
                        visible: text.length > 0
                        font.pointSize: Theme.font.size.small
                        color: Theme.palette.secondaryLabel
                        elide: Text.ElideRight
                    }
                }

                Row {
                    id: buttons

                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 8

                    PillButton {
                        visible: !card.source.installing
                        enabled: !card.source.checking
                        opacity: enabled ? 1 : 0.5
                        text: qsTr("Check Now")
                        onClicked: card.source.check()
                    }

                    PillButton {
                        visible: !card.source.installing && card.source.count > 0
                        primary: true
                        text: card.source.inTerminal ? qsTr("Update in Terminal…") : qsTr("Update Now")
                        onClicked: card.source.install()
                    }
                }
            }

            Rectangle {
                visible: card.source.installing && !card.source.inTerminal
                width: parent.width
                height: 6
                radius: 3
                color: Theme.palette.secondaryFill

                Rectangle {
                    width: parent.width * card.source.progress / 100
                    height: parent.height
                    radius: 3
                    color: Theme.palette.accent

                    Behavior on width {
                        Anim {}
                    }
                }
            }

            StyledText {
                visible: card.source.error !== ""
                width: parent.width
                wrapMode: Text.WordWrap
                text: card.source.error
                font.pointSize: Theme.font.size.small
                color: Theme.palette.red
            }

            Row {
                visible: card.source.restartNeeded && !card.source.installing
                width: parent.width
                spacing: 12

                StyledText {
                    anchors.verticalCenter: parent.verticalCenter
                    width: parent.width - restart.width - 12
                    wrapMode: Text.WordWrap
                    text: qsTr("Restart to finish: the system's core was updated.")
                    font.pointSize: Theme.font.size.small
                }

                PillButton {
                    id: restart

                    primary: true
                    text: qsTr("Restart…")
                    onClicked: Atrium.action("shell", "session:restart")
                }
            }

            // What's newer.
            Column {
                visible: card.source.count > 0 && !card.source.installing
                width: parent.width
                spacing: 0

                Repeater {
                    model: card.source.packages

                    Item {
                        id: pkg

                        required property var modelData
                        required property int index

                        width: parent.width
                        height: 34

                        Rectangle {
                            width: parent.width
                            height: 1
                            color: Theme.palette.separator
                        }

                        StyledText {
                            anchors.left: parent.left
                            anchors.verticalCenter: parent.verticalCenter
                            text: pkg.modelData.name
                        }

                        Row {
                            anchors.right: parent.right
                            anchors.verticalCenter: parent.verticalCenter
                            spacing: 8

                            Rectangle {
                                anchors.verticalCenter: parent.verticalCenter
                                visible: pkg.modelData.security === true
                                width: securityLabel.implicitWidth + 12
                                height: 20
                                radius: 6
                                color: Theme.palette.tertiaryFill

                                StyledText {
                                    id: securityLabel

                                    anchors.centerIn: parent
                                    text: qsTr("Security")
                                    font.pointSize: Theme.font.size.smaller
                                    color: Theme.palette.red
                                }
                            }

                            StyledText {
                                anchors.verticalCenter: parent.verticalCenter
                                text: pkg.modelData.from ? `${pkg.modelData.from} → ${pkg.modelData.version}` : pkg.modelData.version
                                font.pointSize: Theme.font.size.small
                                color: Theme.palette.secondaryLabel
                            }
                        }
                    }
                }
            }
        }
    }

    // --- atrium ----------------------------------------------------------------------
    Rectangle {
        width: parent.width
        height: atriumHead.implicitHeight + 36
        radius: 14
        color: Theme.palette.groupedBackground
        border.width: 1
        border.color: Theme.palette.separator

        Column {
            id: atriumHead

            x: 18
            y: 18
            width: parent.width - 36
            spacing: 12

            Row {
                width: parent.width
                spacing: 14

                Rectangle {
                    width: 44
                    height: 44
                    radius: 12
                    color: AtriumRelease.newer || AtriumRelease.staged ? Theme.palette.accent : Theme.palette.tertiaryFill

                    MaterialIcon {
                        anchors.centerIn: parent
                        text: AtriumRelease.installing ? "downloading" : AtriumRelease.staged ? "restart_alt" : "desktop_windows"
                        fill: 1
                        font.pointSize: Theme.font.size.large + 2
                        color: AtriumRelease.newer || AtriumRelease.staged ? Theme.palette.labelOnAccent : Theme.palette.secondaryLabel
                    }
                }

                Column {
                    anchors.verticalCenter: parent.verticalCenter
                    width: parent.width - 58 - atriumButtons.width
                    spacing: 2

                    StyledText {
                        width: parent.width
                        text: AtriumRelease.installing ? qsTr("Installing atrium %1…").arg(AtriumRelease.latest)
                            : AtriumRelease.staged ? `atrium ${AtriumRelease.stagedVersion} is installed`
                            : AtriumRelease.newer ? `atrium ${AtriumRelease.latest} is available`
                            : `atrium ${AtriumRelease.current}` + (AtriumRelease.latest ? " is up to date" : "")
                        font.weight: Font.DemiBold
                        font.pointSize: Theme.font.size.larger
                        elide: Text.ElideRight
                    }

                    StyledText {
                        width: parent.width
                        text: AtriumRelease.installing ? AtriumRelease.doing
                            : AtriumRelease.staged ? "It takes over when you log out and back in; this session keeps running as it is."
                            : AtriumRelease.checking ? "Checking…"
                            : AtriumRelease.lastChecked ? `Last checked: ${AtriumRelease.lastChecked}` : "From atrium's releases on GitHub"
                        wrapMode: Text.WordWrap
                        font.pointSize: Theme.font.size.small
                        color: Theme.palette.secondaryLabel
                    }
                }

                Row {
                    id: atriumButtons

                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 8

                    PillButton {
                        visible: !AtriumRelease.installing && !AtriumRelease.staged
                        enabled: !AtriumRelease.checking
                        opacity: enabled ? 1 : 0.5
                        text: qsTr("Check Now")
                        onClicked: AtriumRelease.check()
                    }

                    PillButton {
                        visible: !AtriumRelease.installing && AtriumRelease.newer && !AtriumRelease.staged
                        primary: true
                        text: qsTr("Install")
                        onClicked: AtriumRelease.install()
                    }

                    PillButton {
                        visible: AtriumRelease.staged
                        primary: true
                        text: qsTr("Log Out…")
                        onClicked: Atrium.action("shell", "session:logout")
                    }
                }
            }

            Rectangle {
                visible: AtriumRelease.installing
                width: parent.width
                height: 6
                radius: 3
                color: Theme.palette.secondaryFill

                Rectangle {
                    width: parent.width * AtriumRelease.progress / 100
                    height: parent.height
                    radius: 3
                    color: Theme.palette.accent

                    Behavior on width {
                        Anim {}
                    }
                }
            }

            StyledText {
                visible: AtriumRelease.error !== ""
                width: parent.width
                wrapMode: Text.WordWrap
                text: AtriumRelease.error
                font.pointSize: Theme.font.size.small
                color: Theme.palette.red
            }

            // What's new in it, as the release says.
            Column {
                visible: AtriumRelease.newer && AtriumRelease.notes !== "" && !AtriumRelease.staged
                width: parent.width
                spacing: 8

                Rectangle {
                    width: parent.width
                    height: 1
                    color: Theme.palette.separator
                }

                Text {
                    width: parent.width
                    text: AtriumRelease.notes
                    textFormat: Text.MarkdownText
                    wrapMode: Text.WordWrap
                    color: Theme.palette.label
                    linkColor: Theme.palette.accent
                    font.family: Theme.font.sans
                    font.pointSize: Theme.font.size.small
                    onLinkActivated: link => Qt.openUrlExternally(link)
                }
            }
        }
    }

    // --- the system --------------------------------------------------------------------
    MissingNote {
        visible: !Updates.available && !PacmanUpdates.available
        message: qsTr("System updates need PackageKit (packagekit) or pacman's checkupdates (pacman-contrib).")
    }

    SourceCard {
        visible: root.system.available
        source: root.system
        name: "System"
        icon: "system_update_alt"
        idle: "Up to date"
    }

    SourceCard {
        visible: AurUpdates.available
        source: AurUpdates
        name: "AUR"
        icon: "deployed_code"
        idle: "Up to date"
    }
}
