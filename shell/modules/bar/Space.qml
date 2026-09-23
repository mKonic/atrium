pragma ComponentBehavior: Bound

import QtQuick
import Quickshell
import qs.components
import qs.services

// One space in the bar: its number, then an icon for each app on it.
Item {
    id: root

    required property int number
    required property string output
    required property bool active

    readonly property var windows: Atrium.windowsOn(output, number)
    readonly property bool occupied: windows.length > 0
    readonly property int maxIcons: 4

    implicitHeight: Theme.bar.inner - 6
    implicitWidth: Math.max(implicitHeight, content.implicitWidth + Theme.padding.larger * 2)

    Behavior on implicitWidth {
        Anim {
            easing.bezierCurve: Theme.anim.emphasized
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
            color: root.active ? Theme.palette.m3OnPrimary : root.occupied ? Theme.palette.m3OnSurface : Theme.palette.m3Outline
        }

        Repeater {
            // Keyed by window id, so an icon stays put (and doesn't pop in
            // again) when something else changes.
            model: ScriptModel {
                values: root.windows.slice(0, root.maxIcons)
                objectProp: "id"
            }

            MaterialIcon {
                required property var modelData
                readonly property var window: root.windows.find(w => w.id === modelData.id) ?? modelData

                anchors.verticalCenter: parent.verticalCenter
                text: Icons.appCategoryIcon(window.app_id)
                font.pointSize: Theme.font.size.larger
                fill: window.focused ? 1 : 0
                color: root.active ? Theme.palette.m3OnPrimary : Theme.palette.m3OnSurfaceVariant

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
