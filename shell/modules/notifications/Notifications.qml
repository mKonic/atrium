pragma ComponentBehavior: Bound

import QtQuick
import Atrium.Shell
import Atrium
import shell.components
import shell.services

// The notification popups at the top right. atrium's notification server
// (NotificationServer) decides what pops up: Do Not Disturb lets only
// critical ones through, and every one goes into the history.
Scope {
    id: root

    PanelWindow {
        id: stack

        screen: Shell.screen(Atrium.focusedOutput?.name)
        anchors {
            top: true
            right: true
        }
        margins {
            top: 8
            right: 8
        }
        implicitWidth: 380
        implicitHeight: Math.max(1, column.implicitHeight)
        exclusiveZone: 0
        color: "transparent"
        // The notification center, when open, already shows them all.
        visible: NotificationServer.popups.length > 0 && Panels.open !== "notifications"
        WlrLayershell.layer: WlrLayer.Overlay
        WlrLayershell.namespace: "atrium-notifications"

        mask: Region {
            item: column
        }

        Column {
            id: column

            width: parent.width
            spacing: 8

            Repeater {
                model: NotificationServer.popups

                NotificationCard {
                    id: card

                    required property Notification modelData
                    readonly property Notification n: modelData

                    width: column.width
                    app: n.app
                    icon: n.icon
                    image: n.image
                    summary: n.summary
                    body: n.body
                    time: Date.now()
                    critical: n.critical
                    actions: n.actions

                    // Slides in from the right edge.
                    transform: Translate {
                        id: slide

                        x: 0
                    }
                    Component.onCompleted: enter.start()

                    ParallelAnimation {
                        id: enter

                        Anim {
                            target: slide
                            property: "x"
                            from: 400
                            to: 0
                            duration: Theme.anim.normal
                            easing.bezierCurve: Theme.anim.emphasizedDecel
                        }
                        Anim {
                            target: card
                            property: "opacity"
                            from: 0
                            to: 1
                            duration: Theme.anim.small
                        }
                    }

                    // Goes on its own after a while, unless it matters or the
                    // pointer is on it.
                    Timer {
                        running: !card.critical && !card.hovered && !card.expanded
                        interval: card.n.timeout
                        onTriggered: NotificationServer.expire(card.n)
                    }

                    onDismissed: NotificationServer.dismiss(n)
                    onClicked: NotificationServer.activate(n)
                    onAction: id => NotificationServer.invoke(n, id)
                }
            }
        }
    }
}
