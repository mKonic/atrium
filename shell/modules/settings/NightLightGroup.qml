import QtQuick
import Atrium
import shell.components
import shell.services

// Night Light on the Displays page: when (sunset to sunrise, set hours, or
// always) and how warm. The Control Center turns it on or off for now.
Column {
    id: root

    readonly property string mode: Atrium.settings["displays.night_light"] ?? "off"

    spacing: 12

    MissingNote {
        message: (Atrium.nightLight.available ?? true) ? "" : qsTr("These screens can't change their colours.")
        explanation: qsTr("Night Light needs screens atrium drives itself; inside another desktop it does nothing.")
    }

    Group {
        width: parent.width
        title: qsTr("Night Light")

        ControlRow {
            title: qsTr("Schedule")
            note: root.mode === "sunset" ? qsTr("Where your time zone is. %1").arg(Atrium.nightLight.note ?? "") : (Atrium.nightLight.note ?? "")

            Dropdown {
                fieldWidth: 220
                options: [
                    { value: "off", label: qsTr("Off") },
                    { value: "sunset", label: qsTr("Sunset to Sunrise") },
                    { value: "custom", label: qsTr("Custom") },
                    { value: "always", label: qsTr("Always On") }
                ]
                value: root.mode
                onPicked: v => Atrium.setSetting("displays.night_light", v)
            }
        }

        ControlRow {
            visible: root.mode === "custom"
            title: qsTr("From")

            Field {
                width: 100
                text: Atrium.settings["displays.night_light_from"] ?? "22:00"
                placeholder: "22:00"
                onAccepted: Atrium.setSetting("displays.night_light_from", text)
            }
        }

        ControlRow {
            visible: root.mode === "custom"
            title: qsTr("To")

            Field {
                width: 100
                text: Atrium.settings["displays.night_light_to"] ?? "07:00"
                placeholder: "07:00"
                onAccepted: Atrium.setSetting("displays.night_light_to", text)
            }
        }

        ControlRow {
            title: qsTr("Warmth")
            note: qsTr("From a little warmer to a lot.")

            NumberControl {
                min: 0
                max: 100
                value: Atrium.settings["displays.night_light_warmth"] ?? 50
                onCommitted: v => Atrium.setSetting("displays.night_light_warmth", v)
            }
        }
    }
}
