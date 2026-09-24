pragma ComponentBehavior: Bound

import QtQuick
import Atrium
import shell.components
import shell.services

// Which app opens what: links, mail, folders, text, pictures, video,
// music and PDFs (mimeapps.list, which every app and xdg-open read), and
// the terminal the shortcut opens.
Column {
    spacing: 20

    Group {
        Repeater {
            model: DefaultApps.kinds

            ControlRow {
                id: kind

                required property var modelData

                title: modelData.title
                note: modelData.apps.length === 0 ? "No app for this is installed." : ""

                Dropdown {
                    visible: kind.modelData.apps.length > 0
                    fieldWidth: 260
                    options: kind.modelData.apps
                    value: kind.modelData.current
                    onPicked: v => DefaultApps.setDefault(kind.modelData.kind, v)
                }
            }
        }

        ControlRow {
            title: "Terminal"
            note: DefaultApps.terminals.length === 0 ? "No terminal is installed." : ""

            Dropdown {
                visible: DefaultApps.terminals.length > 0
                fieldWidth: 260
                options: DefaultApps.terminals
                value: DefaultApps.terminal
                onPicked: v => DefaultApps.setTerminal(v)
            }
        }
    }
}
