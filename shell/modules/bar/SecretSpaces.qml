pragma ComponentBehavior: Bound

import QtQuick
import qs.components
import qs.services
import Atrium

// Secret spaces next to the numbered ones: a chip for each that holds
// windows (communication: Discord, WhatsApp...), with its apps, lit while it
// is shown over the desktop. Click to show or hide it.
Row {
    id: root

    required property string output

    spacing: Theme.spacing.small

    OutputState {
        id: screenState

        name: root.output
    }

    Repeater {
        model: screenState.secrets

        Pill {
            id: chip

            required property var modelData

            anchors.verticalCenter: parent.verticalCenter
            implicitWidth: row.implicitWidth + Theme.padding.normal * 2
            color: modelData.shown ? Theme.palette.m3Primary : Theme.pill(Theme.palette.m3SurfaceContainer)

            Behavior on color {
                CAnim {
                    duration: Theme.anim.small
                }
            }

            Row {
                id: row

                anchors.centerIn: parent
                spacing: 3

                MaterialIcon {
                    anchors.verticalCenter: parent.verticalCenter
                    text: "visibility_off"
                    font.pointSize: Theme.font.size.normal
                    color: chip.modelData.shown ? Theme.palette.m3OnPrimary : Theme.palette.m3OnSurfaceVariant
                }

                Repeater {
                    model: chip.modelData.apps

                    MaterialIcon {
                        required property string modelData

                        anchors.verticalCenter: parent.verticalCenter
                        text: Icons.appCategoryIcon(modelData)
                        font.pointSize: Theme.font.size.larger
                        color: chip.modelData.shown ? Theme.palette.m3OnPrimary : Theme.palette.m3OnSurfaceVariant
                    }
                }
            }

            TapHandler {
                onTapped: Atrium.toggleSecret(chip.modelData.name)
            }
        }
    }
}
