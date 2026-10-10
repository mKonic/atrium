pragma ComponentBehavior: Bound

import QtQuick
import Atrium.Shell
import shell.components
import shell.modules.settings
import shell.services
import Atrium

// Printing: the printer (or a PDF), copies and pages, then the paper and,
// where the printer can, both sides and colour. Closing it or Cancel prints
// nothing.
FloatingWindow {
    id: root

    title: PrintPrompt.title ? qsTr("Print “%1”").arg(PrintPrompt.title) : qsTr("Print")
    color: Theme.palette.windowBackground
    implicitWidth: 500
    // Room for every row it can show (a printer's seven): it doesn't grow
    // once it's open.
    implicitHeight: 24 + 7 * 32 + 6 * 14 + 84
    minimumSize: Qt.size(440, 360)
    onVisibleChanged: if (!visible) PrintPrompt.cancel()

    Shortcut {
        sequence: "Escape"
        onActivated: PrintPrompt.cancel()
    }

    component Label: StyledText {
        width: 130
        horizontalAlignment: Text.AlignRight
        anchors.verticalCenter: parent?.verticalCenter
        color: Theme.palette.secondaryLabel
    }

    Column {
        id: form

        x: 24
        y: 24
        width: parent.width - 48
        spacing: 14

        Row {
            spacing: 12

            Label {
                text: qsTr("Printer")
            }

            Dropdown {
                fieldWidth: form.width - 142
                options: PrintPrompt.printers
                value: PrintPrompt.printer
                placeholder: qsTr("Looking for printers…")
                onPicked: v => PrintPrompt.printer = v
            }
        }

        Row {
            visible: PrintPrompt.pdf
            spacing: 12

            Label {
                text: qsTr("Save as")
            }

            Field {
                width: form.width - 142
                text: PrintPrompt.pdfFile
                onTextChanged: PrintPrompt.pdfFile = text
                onAccepted: PrintPrompt.print()
            }
        }

        Row {
            spacing: 12

            Label {
                text: qsTr("Copies")
            }

            Row {
                spacing: 6

                PillButton {
                    text: "−"
                    enabled: PrintPrompt.copies > 1
                    opacity: enabled ? 1 : 0.5
                    onClicked: PrintPrompt.copies = PrintPrompt.copies - 1
                }

                StyledText {
                    width: 36
                    anchors.verticalCenter: parent.verticalCenter
                    horizontalAlignment: Text.AlignHCenter
                    text: PrintPrompt.copies
                    font.weight: Font.DemiBold
                }

                PillButton {
                    text: "+"
                    enabled: PrintPrompt.copies < 999
                    opacity: enabled ? 1 : 0.5
                    onClicked: PrintPrompt.copies = PrintPrompt.copies + 1
                }
            }
        }

        Row {
            spacing: 12

            Label {
                text: qsTr("Pages")
            }

            Field {
                width: 200
                placeholder: qsTr("All")
                text: PrintPrompt.pages
                onTextChanged: PrintPrompt.pages = text
                onAccepted: PrintPrompt.print()
            }
        }

        Row {
            spacing: 12

            Label {
                text: qsTr("Paper size")
            }

            Dropdown {
                fieldWidth: 220
                options: PrintPrompt.papers
                value: PrintPrompt.paper
                placeholder: qsTr("The printer's")
                onPicked: v => PrintPrompt.paper = v
            }
        }

        Row {
            spacing: 12

            Label {
                text: qsTr("Orientation")
            }

            ChoiceControl {
                choices: ["portrait", "landscape"]
                value: PrintPrompt.landscape ? "landscape" : "portrait"
                onPicked: v => PrintPrompt.landscape = v === "landscape"
            }
        }

        Row {
            visible: PrintPrompt.duplexes.length > 0
            spacing: 12

            Label {
                text: qsTr("Two-sided")
            }

            Dropdown {
                fieldWidth: 220
                options: PrintPrompt.duplexes
                value: PrintPrompt.duplex
                onPicked: v => PrintPrompt.duplex = v
            }
        }

        Row {
            visible: PrintPrompt.colors.length > 0
            spacing: 12

            Label {
                text: qsTr("Colour")
            }

            Dropdown {
                fieldWidth: 220
                options: PrintPrompt.colors
                value: PrintPrompt.color
                onPicked: v => PrintPrompt.color = v
            }
        }
    }

    StyledText {
        anchors.left: parent.left
        anchors.leftMargin: 24
        anchors.right: buttons.left
        anchors.rightMargin: 12
        anchors.verticalCenter: buttons.verticalCenter
        wrapMode: Text.Wrap
        text: PrintPrompt.problem
        color: PrintPrompt.printer ? Theme.palette.red : Theme.palette.secondaryLabel
        font.pointSize: Theme.font.size.smaller
    }

    Row {
        id: buttons

        anchors.right: parent.right
        anchors.rightMargin: 24
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 20
        spacing: 10

        PillButton {
            text: qsTr("Cancel")
            onClicked: PrintPrompt.cancel()
        }

        PillButton {
            primary: true
            enabled: PrintPrompt.problem === ""
            opacity: enabled ? 1 : 0.5
            text: PrintPrompt.pdf ? qsTr("Save") : PrintPrompt.acceptLabel
            onClicked: PrintPrompt.print()
        }
    }
}
