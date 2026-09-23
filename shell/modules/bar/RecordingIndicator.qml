import QtQuick
import qs.components
import qs.services
import Atrium

// While the screen is being recorded: how long, and a click stops it.
Pill {
    id: root

    visible: Recorder.recording
    implicitWidth: row.implicitWidth + Theme.padding.normal * 2
    color: "#93000a"

    Row {
        id: row

        anchors.centerIn: parent
        spacing: 6

        Rectangle {
            anchors.verticalCenter: parent.verticalCenter
            width: 8
            height: 8
            radius: 4
            color: "#ffb4ab"

            SequentialAnimation on opacity {
                running: Recorder.recording
                loops: Animation.Infinite

                Anim {
                    to: 0.3
                    duration: 700
                }
                Anim {
                    to: 1
                    duration: 700
                }
            }
        }

        StyledText {
            anchors.verticalCenter: parent.verticalCenter
            text: `${Math.floor(Recorder.seconds / 60)}:${String(Recorder.seconds % 60).padStart(2, "0")}`
            font.pointSize: Theme.font.size.smaller
            font.weight: Font.DemiBold
            color: "#ffdad6"
        }
    }

    TapHandler {
        onTapped: Recorder.stop()
    }
}
