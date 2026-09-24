pragma ComponentBehavior: Bound

import QtQuick
import Atrium.Shell
import shell.components
import shell.services
import Atrium

// One space in the bar: its number, then an icon for each app on it.
Item {
    id: root

    required property int number
    required property string output
    required property bool active
    // Drawn in the highlight's colours: the copy of the row that shows
    // through the sliding highlight (see Spaces).
    property bool inverted: false

    readonly property var windows: onSpace.windows

    SpaceWindows {
        id: onSpace

        output: root.output
        number: root.number
    }
    readonly property bool occupied: windows.length > 0
    readonly property int maxIcons: 4

    implicitHeight: Theme.bar.inner - 6
    implicitWidth: Math.max(implicitHeight, content.implicitWidth + Theme.padding.larger * 2)

    Behavior on implicitWidth {
        SmoothedAnimation {
            velocity: -1
            duration: Theme.anim.normal
        }
    }

    Row {
        id: content

        anchors.centerIn: parent
        spacing: Theme.spacing.small

        StyledText {
            anchors.verticalCenter: parent.verticalCenter
            text: root.number
            font.pointSize: Theme.font.size.normal
            font.weight: root.active ? Font.DemiBold : Font.Medium
            color: root.inverted ? Theme.palette.labelOnAccent : root.occupied ? Theme.palette.label : Theme.palette.tertiaryLabel
        }

        Repeater {
            // Keyed by window id, so an icon stays put (and doesn't pop in
            // again) when something else changes.
            model: KeyedModel {
                values: root.windows
                key: "id"
                limit: root.maxIcons
            }

            MaterialIcon {
                required property var modelData
                readonly property var window: modelData

                anchors.verticalCenter: parent?.verticalCenter
                text: Icons.appCategoryIcon(window.app_id)
                font.pointSize: Theme.font.size.larger
                fill: window.focused ? 1 : 0
                color: root.inverted ? Theme.palette.labelOnAccent : Theme.palette.secondaryLabel

                // Pops in when the app opens.
                Anim on scale {
                    from: 0
                    to: 1
                    easing.bezierCurve: Theme.anim.emphasizedDecel
                }
            }
        }
    }
}
