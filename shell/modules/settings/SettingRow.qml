import QtQuick
import shell.components
import shell.services
import Atrium

// One setting: its name and what it does on the left, its control on the
// right (or, for lists, shortcuts and rules, an editor underneath).
Item {
    id: root

    required property var setting
    property bool showPage: false

    readonly property string key: setting.key
    readonly property string type: setting.type
    readonly property var value: Atrium.settings[key] ?? setting.default
    readonly property bool changed: JSON.stringify(value) !== JSON.stringify(setting.default)
    readonly property bool wide: type === "list"
    // Why it can't work here ("cliphist isn't installed."), if it can't.
    readonly property string missing: Requirements.missing[setting.needs ?? ""] ?? ""

    function set(v: var): void {
        Atrium.setSetting(key, v);
    }

    implicitHeight: Math.max(56, head.implicitHeight + 24) + (wide ? editor.height + 12 : 0)

    Column {
        id: head

        x: 16
        y: 12
        width: parent.width - 32 - (root.wide ? 0 : control.width + 24) - reset.width
        spacing: 2

        StyledText {
            visible: root.showPage
            text: root.setting.page
            font.pointSize: Theme.font.size.smaller
            color: Theme.palette.m3Primary
        }

        StyledText {
            width: parent.width
            text: root.setting.title
            wrapMode: Text.WordWrap
            font.weight: Font.Medium
        }

        StyledText {
            width: parent.width
            visible: text.length > 0
            text: root.setting.description
            wrapMode: Text.WordWrap
            font.pointSize: Theme.font.size.small
            color: Theme.palette.m3OnSurfaceVariant
        }

        StyledText {
            width: parent.width
            visible: root.missing.length > 0
            text: root.missing
            wrapMode: Text.WordWrap
            font.pointSize: Theme.font.size.small
            font.weight: Font.Medium
            color: Theme.palette.m3OnSurface
        }
    }

    // Back to the default, when it isn't.
    MaterialIcon {
        id: reset

        anchors.right: root.wide ? parent.right : control.left
        anchors.rightMargin: root.wide ? 16 : 10
        y: root.wide ? 14 : (Math.max(56, head.implicitHeight + 24) - height) / 2
        visible: root.changed
        width: visible ? implicitWidth : 0
        text: "restart_alt"
        font.pointSize: Theme.font.size.normal
        color: resetArea.containsMouse ? Theme.palette.m3OnSurface : Theme.palette.m3OnSurfaceVariant

        MouseArea {
            id: resetArea

            anchors.fill: parent
            anchors.margins: -6
            hoverEnabled: true
            onClicked: Atrium.resetSetting(root.key)
        }
    }

    Loader {
        id: control

        anchors.right: parent.right
        anchors.rightMargin: 16
        y: (Math.max(56, head.implicitHeight + 24) - height) / 2
        active: !root.wide
        enabled: root.missing.length === 0
        opacity: enabled ? 1 : 0.4
        width: item?.implicitWidth ?? 0
        height: item?.implicitHeight ?? 0
        sourceComponent: root.type === "bool" ? boolControl
                       : root.type === "int" || root.type === "float" ? numberControl
                       : root.key === "appearance.accent" ? accentControl
                       : root.key === "appearance.wallpaper" ? wallpaperControl
                       : Themes.optionsFor(root.key).length > 0 ? themeControl
                       : root.type === "choice" && (root.setting.choices?.length ?? 0) > 5 ? dropdownControl
                       : root.type === "choice" ? choiceControl
                       : root.type === "color" ? colorControl
                       : textControl
    }

    Loader {
        id: editor

        x: 16
        y: Math.max(56, head.implicitHeight + 24)
        width: parent.width - 32
        height: item?.implicitHeight ?? 0
        active: root.wide
        sourceComponent: listEditor
    }

    Component {
        id: boolControl

        Switch {
            checked: root.value === true
            onToggled: root.set(!checked)
        }
    }

    Component {
        id: numberControl

        NumberControl {
            value: Number(root.value)
            min: root.setting.min ?? 0
            max: root.setting.max ?? 100
            integer: root.type === "int"
            onCommitted: v => root.set(v)
        }
    }

    Component {
        id: choiceControl

        ChoiceControl {
            value: String(root.value)
            choices: root.setting.choices ?? []
            onPicked: v => root.set(v)
        }
    }

    // Installed icon themes, cursors and fonts.
    Component {
        id: themeControl

        Dropdown {
            fieldWidth: 220
            value: String(root.value ?? "")
            placeholder: "Default"
            options: Themes.optionsFor(root.key)
            onPicked: v => root.set(v)
        }
    }

    // Many choices: a pop-up menu, as macOS has them.
    Component {
        id: dropdownControl

        Dropdown {
            fieldWidth: 180
            value: String(root.value)
            options: SettingsPages.choiceOptions(root.setting.choices ?? [])
            onPicked: v => root.set(v)
        }
    }

    Component {
        id: accentControl

        AccentControl {
            value: String(root.value)
            choices: root.setting.choices ?? []
            onPicked: v => root.set(v)
        }
    }

    Component {
        id: wallpaperControl

        WallpaperControl {
            value: String(root.value ?? "")
            onPicked: file => Wallpaper.set(file)
        }
    }

    Component {
        id: colorControl

        ColorControl {
            value: String(root.value)
            onCommitted: v => root.set(v)
        }
    }

    Component {
        id: textControl

        TextControl {
            value: String(root.value ?? "")
            onCommitted: v => root.set(v)
        }
    }

    Component {
        id: listEditor

        ListEditor {
            value: root.value ?? []
            onCommitted: v => root.set(v)
        }
    }

}
