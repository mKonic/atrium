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
        // As tall as it may ever need, not as its cards: resizing the window
        // with every frame of a card growing makes it flicker (each size is a
        // round trip to the compositor). Only the cards take input and glass.
        implicitHeight: (screen?.height ?? 900) - 16
        exclusiveZone: 0
        color: "transparent"
        // The notification center, when open, already shows them all.
        visible: NotificationServer.popupCount > 0 && Panels.open !== "notifications"
        WlrLayershell.layer: WlrLayer.Overlay
        WlrLayershell.namespace: "atrium-notifications"

        mask: Region {
            item: list
        }

        // A new one slides in at the top from the right and the others move
        // down to make room; one leaving slides back out, and the rest close up.
        ListView {
            id: list

            width: parent.width
            height: contentHeight
            spacing: 8
            interactive: false
            model: NotificationServer.popups

            add: Transition {
                ParallelAnimation {
                    Anim {
                        property: "x"
                        from: 400
                        to: 0
                        duration: Theme.anim.normal
                        easing.bezierCurve: Theme.anim.emphasizedDecel
                    }
                    Anim {
                        property: "opacity"
                        from: 0
                        to: 1
                        duration: Theme.anim.small
                    }
                }
            }
            remove: Transition {
                ParallelAnimation {
                    Anim {
                        property: "x"
                        to: 400
                        duration: Theme.anim.small
                        easing.bezierCurve: Theme.anim.emphasizedAccel
                    }
                    Anim {
                        property: "opacity"
                        to: 0
                        duration: Theme.anim.small
                    }
                }
            }
            displaced: Transition {
                Anim {
                    properties: "x,y"
                    duration: Theme.anim.normal
                    easing.bezierCurve: Theme.anim.standard
                }
            }

            delegate: NotificationCard {
                id: card

                required property Notification notification
                required property int index
                readonly property Notification n: notification

                z: -index  // the newest over the ones moving down past it

                width: list.width
                app: n.app
                icon: n.icon
                image: n.image
                summary: n.summary
                body: n.body
                time: Date.now()
                critical: n.critical
                actions: n.actions

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
