pragma ComponentBehavior: Bound

import QtQuick
import shell.components
import shell.services
import Atrium

// Spaces as caelestia does workspaces: a group of five at a time (paging on
// as you go past them), each with its apps' icons, and a pill that slides to
// the one you're on. Click to go, scroll to step.
Pill {
    id: root

    required property string output
    readonly property int shown: 5
    readonly property int active: screenState.activeSpace

    OutputState {
        id: screenState

        name: root.output
    }
    readonly property int groupOffset: Math.floor((active - 1) / shown) * shown

    implicitWidth: row.implicitWidth + 6

    Rectangle {
        id: indicator

        readonly property Item target: spaces.count > 0 ? spaces.itemAt(root.active - root.groupOffset - 1) : null

        x: row.x + (target?.x ?? 0)
        y: row.y
        width: target?.width ?? 0
        height: row.height
        radius: Theme.rounding.full
        color: Theme.palette.accent
        visible: target !== null
        clip: true
        z: 1  // over the row: its copy is what shows where it covers

        // The same row again in the highlight's colours, lined up with the
        // real one: whatever the highlight covers turns, crisply, even
        // halfway through a slide (caelestia's Colouriser).
        Row {
            x: row.x - indicator.x
            y: row.y - indicator.y

            Repeater {
                model: root.shown

                Space {
                    required property int index

                    number: root.groupOffset + index + 1
                    output: root.output
                    active: number === root.active
                    inverted: true
                }
            }
        }

        // Smoothed rather than restarted: apps opening and closing change
        // the spaces' widths many times in a row, and a fresh eased animation
        // for each change stutters; this one keeps its speed and retargets.
        Behavior on x {
            SmoothedAnimation {
                velocity: -1
                duration: Theme.anim.normal
            }
        }

        Behavior on width {
            SmoothedAnimation {
                velocity: -1
                duration: Theme.anim.normal
            }
        }
    }

    Row {
        id: row

        anchors.centerIn: parent

        Repeater {
            id: spaces

            model: root.shown

            Space {
                required property int index

                number: root.groupOffset + index + 1
                output: root.output
                active: number === root.active
            }
        }
    }

    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.LeftButton

        onClicked: event => {
            const item = row.childAt(event.x - row.x, event.y - row.y);
            if (item && item.number !== undefined && item.number !== root.active)
                Atrium.switchSpace(item.number);
        }

        onWheel: event => {
            const next = root.active + (event.angleDelta.y < 0 ? 1 : -1);
            if (next >= 1)
                Atrium.switchSpace(next);
        }
    }
}
