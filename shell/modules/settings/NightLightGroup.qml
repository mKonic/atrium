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
        message: (Atrium.nightLight.available ?? true) ? "" : "These screens can't change their colours."
        explanation: "Night Light needs screens atrium drives itself; inside another desktop it does nothing."
    }

    Group {
        width: parent.width
        title: "Night Light"

        ControlRow {
            title: "Schedule"
            note: root.mode === "sunset" ? `Where your time zone is. ${Atrium.nightLight.note ?? ""}` : (Atrium.nightLight.note ?? "")

            Dropdown {
                fieldWidth: 220
                options: [
                    { value: "off", label: "Off" },
                    { value: "sunset", label: "Sunset to Sunrise" },
                    { value: "custom", label: "Custom" },
                    { value: "always", label: "Always On" }
                ]
                value: root.mode
                onPicked: v => Atrium.setSetting("displays.night_light", v)
            }
        }

        ControlRow {
            visible: root.mode === "custom"
            title: "From"

            Field {
                width: 100
                text: Atrium.settings["displays.night_light_from"] ?? "22:00"
                placeholder: "22:00"
                onAccepted: Atrium.setSetting("displays.night_light_from", text)
            }
        }

        ControlRow {
            visible: root.mode === "custom"
            title: "To"

            Field {
                width: 100
                text: Atrium.settings["displays.night_light_to"] ?? "07:00"
                placeholder: "07:00"
                onAccepted: Atrium.setSetting("displays.night_light_to", text)
            }
        }

        ControlRow {
            title: "Warmth"
            note: "From a little warmer to a lot."

            NumberControl {
                min: 0
                max: 100
                value: Atrium.settings["displays.night_light_warmth"] ?? 50
                onCommitted: v => Atrium.setSetting("displays.night_light_warmth", v)
            }
        }
    }
}
