pragma ComponentBehavior: Bound

import QtQuick
import Atrium
import shell.components
import shell.services

// Printers, as macOS lists them: each with its state and what's waiting to
// print, the default ringed, the network's own printers ready as they are,
// and one to add by its address.
Column {
    id: root

    property string error: ""

    spacing: 20

    Binding {
        when: root.visible && !Requirements.missing.cups
        target: Printers
        property: "watching"
        value: true
        restoreMode: Binding.RestoreValue
    }

    Connections {
        target: Printers

        function onFailed(why: string): void {
            root.error = why;
        }
    }

    MissingNote {
        needs: "cups"
        explanation: "Printing goes through CUPS (the cups package, with cups.socket enabled)."
    }

    SectionHeader {
        visible: !Requirements.missing.cups
        width: parent.width
        subtitle: root.error || (Printers.printers.length === 0 ? "No printers yet. Ones on your network show up by themselves." : "")

        PillButton {
            text: "Add…"
            icon: "add"
            onClicked: addSheet.open()
        }
    }

    Repeater {
        model: Requirements.missing.cups ? [] : Printers.printers

        Group {
            id: printer

            required property var modelData

            DeviceRow {
                glyph: "print"
                name: printer.modelData.label
                note: [printer.modelData.isDefault ? "Default" : "",
                       printer.modelData.state === "printing" ? "Printing" : printer.modelData.state === "stopped" ? "Stopped" : "Idle",
                       printer.modelData.installed ? "" : "On the network"].filter(s => s).join(" · ")
                active: printer.modelData.isDefault

                PillButton {
                    visible: !printer.modelData.isDefault
                    text: "Make Default"
                    onClicked: {
                        root.error = "";
                        Printers.setDefault(printer.modelData.name);
                    }
                }

                PillButton {
                    visible: printer.modelData.installed
                    icon: "remove"
                    onClicked: {
                        root.error = "";
                        Printers.remove(printer.modelData.name);
                    }
                }
            }

            Repeater {
                model: printer.modelData.jobs

                ControlRow {
                    id: job

                    required property var modelData

                    title: modelData.id
                    note: `${modelData.user} · ${modelData.size}`

                    PillButton {
                        text: "Cancel"
                        onClicked: Printers.cancel(job.modelData.id)
                    }
                }
            }
        }
    }

    Sheet {
        id: addSheet

        title: "Add a Printer"
        action: "Add"
        ready: printerName.text.trim().length > 0 && address.text.trim().length > 0
        onOpened: {
            printerName.text = address.text = "";
            address.focusField();
        }
        onSubmitted: {
            root.error = "";
            Printers.add(printerName.text, address.text);
            close();
        }

        Field {
            id: address

            width: parent.width
            placeholder: "Address (192.168.1.20 or ipp://…)"
        }

        Field {
            id: printerName

            width: parent.width
            placeholder: "Name"
        }

        StyledText {
            width: parent.width
            wrapMode: Text.WordWrap
            text: "For printers that print without a driver (AirPrint, IPP Everywhere): most made since 2015."
            font.pointSize: Theme.font.size.small
            color: Theme.palette.secondaryLabel
        }
    }
}
