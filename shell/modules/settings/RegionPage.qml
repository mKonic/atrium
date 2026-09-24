import QtQuick
import Atrium
import shell.components
import shell.services

// The language apps speak and the formats they write dates, numbers and
// money in, for everyone on this computer (localed), from the next login.
Column {
    id: root

    property string error: ""  // why the last change didn't happen

    spacing: 20

    Connections {
        target: LocaleSettings

        function onFailed(why: string): void {
            root.error = why;
        }
    }

    Group {
        subtitle: root.error || "For everyone on this computer. Apps use them from the next login."

        ControlRow {
            title: "Language"

            Dropdown {
                fieldWidth: 300
                options: LocaleSettings.locales
                value: LocaleSettings.language
                onPicked: v => LocaleSettings.setLanguage(v)
            }
        }

        ControlRow {
            title: "Formats"
            note: LocaleSettings.sample

            Dropdown {
                fieldWidth: 300
                options: LocaleSettings.locales
                value: LocaleSettings.formats
                onPicked: v => LocaleSettings.setFormats(v)
            }
        }
    }
}
