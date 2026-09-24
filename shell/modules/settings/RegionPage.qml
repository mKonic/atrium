import QtQuick
import Atrium
import shell.components
import shell.services

// The language apps speak and the formats they write dates, numbers and
// money in, for everyone on this computer (localed), from the next login.
Column {
    spacing: 20

    Group {
        subtitle: "For everyone on this computer. Apps use them from the next login."

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
