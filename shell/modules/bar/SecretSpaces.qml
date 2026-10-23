pragma ComponentBehavior: Bound

import QtQuick
import shell.components
import shell.services
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

            anchors.verticalCenter: parent?.verticalCenter
            implicitWidth: row.implicitWidth + Theme.padding.normal * 2
            color: modelData.shown ? Theme.bar.palette.accent : Theme.lens ? Theme.bar.pill : Theme.bar.palette.fill

            Glass {}

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
                    anchors.verticalCenter: parent?.verticalCenter
                    text: "visibility_off"
                    font.pointSize: Theme.font.size.normal
                    color: chip.modelData.shown ? Theme.bar.palette.labelOnAccent : Theme.bar.palette.secondaryLabel
                }

                Repeater {
                    model: chip.modelData.apps

                    MaterialIcon {
                        required property string modelData

                        anchors.verticalCenter: parent?.verticalCenter
                        text: Icons.appCategoryIcon(modelData)
                        font.pointSize: Theme.font.size.larger
                        color: chip.modelData.shown ? Theme.bar.palette.labelOnAccent : Theme.bar.palette.secondaryLabel
                    }
                }
            }

            TapHandler {
                onTapped: Atrium.toggleSecret(chip.modelData.name)
            }
        }
    }
}
