pragma ComponentBehavior: Bound

import QtCore
import QtQuick
import QtQuick.Dialogs
import QtQuick.Layouts
import Atrium.Shell
import shell.components
import shell.modules.settings
import shell.services
import Atrium

// The first login's welcome, as macOS's Setup Assistant: a few pages that
// each set one thing (all of it also in System Settings), with Skip always
// there. Closing it any way counts as done.
FloatingWindow {
    id: root

    property int page: 0
    readonly property var pages: ["welcome", "appearance", "keyboard", "apps", "wallpaper", "done"]
    readonly property bool last: page === pages.length - 1

    function finish(): void {
        Welcome.done();
        Qt.quit();
    }

    title: "Welcome"
    color: Theme.palette.windowBackground
    implicitWidth: 720
    implicitHeight: 560
    minimumSize: Qt.size(720, 560)
    onVisibleChanged: if (!visible) finish()

    StackLayout {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.bottom: footer.top
        anchors.margins: 40
        currentIndex: root.page

        // --- welcome -------------------------------------------------------
        Column {
            spacing: 14

            Item {
                width: 1
                height: 70
            }

            IconImage {
                anchors.horizontalCenter: parent.horizontalCenter
                implicitSize: 96
                source: Shell.iconPath("atrium-welcome")
            }

            StyledText {
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                text: "Welcome to atrium"
                font.pointSize: 26
                font.weight: Font.Bold
            }

            StyledText {
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
                text: "A few choices to make it yours. Everything here is also in System Settings, so nothing is final."
                color: Theme.palette.secondaryLabel
            }
        }

        // --- appearance ----------------------------------------------------
        Page {
            title: "Choose your look"
            subtitle: "Light or dark, and the colour of highlights and buttons."

            Row {
                anchors.horizontalCenter: parent.horizontalCenter
                spacing: 28

                Repeater {
                    model: ["light", "dark"]

                    LookCard {
                        required property string modelData

                        style: modelData
                        chosen: (Atrium.settings["appearance.style"] ?? "dark") === modelData
                        onClicked: Atrium.setSetting("appearance.style", modelData)
                    }
                }
            }

            Item {
                width: 1
                height: 10
            }

            Rows {
                Line {
                    title: "Accent colour"

                    AccentControl {
                        value: Atrium.settings["appearance.accent"] ?? "multicolor"
                        choices: Atrium.schema.find(s => s.key === "appearance.accent")?.choices ?? []
                        onPicked: v => Atrium.setSetting("appearance.accent", v)
                    }
                }

                Line {
                    title: "Liquid Glass"
                    note: "The bar, Dock and panels as clear glass over a vivid blur."

                    Switch {
                        checked: Atrium.settings["appearance.liquid_glass"] ?? false
                        onToggled: Atrium.setSetting("appearance.liquid_glass", !checked)
                    }
                }
            }
        }

        // --- keyboard ------------------------------------------------------
        Page {
            title: "Your keyboard"
            subtitle: "The layout of the keys you type on. More layouts, and switching between them, are in System Settings, Keyboard."

            Rows {
                Line {
                    title: "Layout"

                    Dropdown {
                        fieldWidth: 260
                        options: KeyboardLayouts.layouts
                        value: KeyboardLayouts.current
                        onPicked: v => KeyboardLayouts.set(v)
                    }
                }
            }

            Field {
                width: parent.width
                placeholder: "Try it here"
            }
        }

        // --- apps ----------------------------------------------------------
        Page {
            title: "Your apps"
            subtitle: "What opens links, and what the terminal shortcut opens."

            Rows {
                Line {
                    title: "Web browser"
                    note: DefaultApps.browsers.length === 0 ? "No web browser is installed." : ""

                    Dropdown {
                        visible: DefaultApps.browsers.length > 0
                        fieldWidth: 260
                        options: DefaultApps.browsers
                        value: DefaultApps.browser
                        onPicked: v => DefaultApps.setBrowser(v)
                    }
                }

                Line {
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

        // --- wallpaper -----------------------------------------------------
        Page {
            title: "A wallpaper"
            subtitle: Wallpaper.found ? `You have one from ${Wallpaper.foundFrom}; bring it along, or choose another.` : "Choose a picture for the desktop, or keep the plain colour."

            Rectangle {
                anchors.horizontalCenter: parent.horizontalCenter
                width: 320
                height: 200
                radius: 12
                color: Atrium.settings["appearance.background"] ?? Theme.palette.tertiaryFill
                border.width: 1
                border.color: Theme.palette.separator
                clip: true

                Image {
                    anchors.fill: parent
                    anchors.margins: 1
                    source: Wallpaper.url(Atrium.settings["appearance.wallpaper"] ?? "")
                    sourceSize: Qt.size(640, 400)
                    fillMode: Image.PreserveAspectCrop
                    asynchronous: true
                }
            }

            Row {
                anchors.horizontalCenter: parent.horizontalCenter
                spacing: 10

                PillButton {
                    visible: Wallpaper.found !== "" && Wallpaper.found !== (Atrium.settings["appearance.wallpaper"] ?? "")
                    primary: true
                    text: `Use my ${Wallpaper.foundFrom} wallpaper`
                    onClicked: Wallpaper.setPath(Wallpaper.found)
                }

                PillButton {
                    text: "Choose…"
                    onClicked: picker.open()
                }
            }

            FileDialog {
                id: picker

                title: "Choose a wallpaper"
                currentFolder: StandardPaths.writableLocation(StandardPaths.PicturesLocation)
                nameFilters: ["Pictures (*.png *.jpg *.jpeg *.webp *.avif *.jxl *.bmp)"]
                onAccepted: Wallpaper.set(selectedFile)
            }
        }

        // --- done ----------------------------------------------------------
        Column {
            spacing: 14

            Item {
                width: 1
                height: 90
            }

            MaterialIcon {
                anchors.horizontalCenter: parent.horizontalCenter
                text: "check_circle"
                font.pointSize: 54
                color: Theme.palette.accent
            }

            StyledText {
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                text: "You're all set"
                font.pointSize: 26
                font.weight: Font.Bold
            }

            StyledText {
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
                text: "Super+Space finds apps and files, and System Settings (Super+,) has the rest."
                color: Theme.palette.secondaryLabel
            }
        }
    }

    // --- the buttons -------------------------------------------------------
    Item {
        id: footer

        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: 72

        Rectangle {
            width: parent.width
            height: 1
            color: Theme.palette.separator
        }

        PillButton {
            anchors.left: parent.left
            anchors.leftMargin: 24
            anchors.verticalCenter: parent.verticalCenter
            visible: !root.last
            text: "Skip"
            onClicked: root.finish()
        }

        Row {
            anchors.right: parent.right
            anchors.rightMargin: 24
            anchors.verticalCenter: parent.verticalCenter
            spacing: 10

            PillButton {
                visible: root.page > 0
                text: "Back"
                onClicked: root.page -= 1
            }

            PillButton {
                primary: true
                text: root.last ? "Get Started" : "Continue"
                onClicked: root.last ? root.finish() : root.page += 1
            }
        }
    }

    // A page: a title, a line under it, then its content.
    component Page: Column {
        id: pageColumn

        property string title
        property string subtitle

        spacing: 18

        StyledText {
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            text: pageColumn.title
            font.pointSize: 22
            font.weight: Font.Bold
        }

        StyledText {
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
            text: pageColumn.subtitle
            color: Theme.palette.secondaryLabel
        }

        Item {
            width: 1
            height: 6
        }
    }

    // A rounded card of rows, as in System Settings.
    component Rows: Rectangle {
        default property alias rows: rowsColumn.data

        width: parent.width
        height: rowsColumn.implicitHeight
        radius: 14
        color: Theme.palette.tertiaryFill
        border.width: 1
        border.color: Theme.palette.separator

        Column {
            id: rowsColumn

            width: parent.width
        }
    }

    // A row: a name (and a note) on the left, its control on the right.
    component Line: Item {
        id: line

        property string title
        property string note
        default property alias control: slot.data

        width: parent.width
        implicitHeight: Math.max(56, labels.implicitHeight + 24, slot.childrenRect.height + 24)

        Column {
            id: labels

            x: 16
            anchors.verticalCenter: parent.verticalCenter
            width: parent.width - 32 - slot.childrenRect.width - 16
            spacing: 2

            StyledText {
                width: parent.width
                text: line.title
                font.weight: Font.Medium
            }

            StyledText {
                width: parent.width
                visible: line.note.length > 0
                text: line.note
                wrapMode: Text.WordWrap
                font.pointSize: Theme.font.size.small
                color: Theme.palette.secondaryLabel
            }
        }

        Item {
            id: slot

            anchors.right: parent.right
            anchors.rightMargin: 16
            anchors.verticalCenter: parent.verticalCenter
            width: childrenRect.width
            height: childrenRect.height
        }
    }
}
