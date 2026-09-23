pragma ComponentBehavior: Bound

import QtQuick
import Quickshell
import Quickshell.Wayland
import qs.components
import qs.services
import Atrium

// Every shortcut: its keys (click to record new ones), what it does, and
// what with. While recording, atrium lets the keys through to this window.
Rectangle {
    id: root

    required property var window
    property var recording: null  // the shortcut id being recorded

    // What a shortcut can do, in words; and whether it needs an argument.
    readonly property var labels: ({
            "spawn": "Run a command", "terminal": "Open the terminal", "close": "Close window",
            "fullscreen": "Toggle fullscreen", "maximize": "Maximize", "minimize": "Minimize",
            "focus-next": "Focus next window", "focus-prev": "Focus previous window",
            "switch-vt": "Switch to console", "quit": "Log out", "space": "Go to space",
            "move-to-space": "Move window to space", "space-prev": "Previous space", "space-next": "Next space",
            "toggle-secret": "Show secret space", "move-to-secret": "Move window to secret space",
            "snap-left": "Snap left", "snap-right": "Snap right", "restore": "Restore window",
            "overview": "Mission Control", "switch-next": "Switch windows", "switch-prev": "Switch windows backwards",
            "cycle-space-next": "Cycle spaces", "cycle-space-prev": "Cycle spaces backwards",
            "restart-shell": "Restart the shell", "shell": "Shell"
        })
    readonly property var argHints: ({
            "spawn": "Command", "space": "Number", "move-to-space": "Number", "switch-vt": "Number",
            "toggle-secret": "Space name", "move-to-secret": "Space name", "shell": "What"
        })

    function add(): void {
        Atrium.addShortcut({ keys: "Mod+F12", action: "terminal" });
    }

    width: parent?.width ?? 0
    height: column.implicitHeight + 32
    radius: 14
    color: Theme.palette.m3SurfaceContainer
    border.width: 1
    border.color: Theme.alpha(Theme.palette.m3Outline, 0.12)

    // The keys reach this window, not atrium's own shortcuts, while recording.
    ShortcutInhibitor {
        window: root.window
        enabled: root.recording !== null
    }

    Item {
        id: catcher

        focus: root.recording !== null
        Keys.onPressed: event => {
            event.accepted = true;
            if (event.key === Qt.Key_Escape && event.modifiers === Qt.NoModifier) {
                root.recording = null;
                return;
            }
            const keys = SettingsPages.chord(event.key, event.modifiers, Atrium.settings["shortcuts.modifier"] ?? "super");
            if (!keys)
                return;  // a modifier on its own: wait for the key
            Atrium.setShortcut(root.recording, { keys: keys });
            root.recording = null;
        }
    }

    Column {
        id: column

        x: 16
        y: 16
        width: parent.width - 32
        spacing: 4

        SectionHeader {
            width: parent.width
            title: "Shortcuts"
            subtitle: "Click a shortcut's keys and press new ones. Esc cancels."

            PillButton {
                text: "Restore Defaults"
                onClicked: Atrium.resetShortcuts()
            }

            PillButton {
                text: "Add"
                icon: "add"
                primary: true
                onClicked: root.add()
            }
        }

        Item {
            width: 1
            height: 8
        }

        Repeater {
            model: Atrium.shortcuts

            Item {
                id: row

                required property var modelData
                required property int index
                readonly property string hint: root.argHints[modelData.action] ?? ""

                width: column.width
                height: 44

                Rectangle {
                    visible: row.index > 0
                    width: parent.width
                    height: 1
                    color: Theme.alpha(Theme.palette.m3Outline, 0.12)
                }

                Item {
                    anchors.left: parent.left
                    anchors.verticalCenter: parent.verticalCenter
                    width: 230
                    height: 30

                    KeyCaps {
                        anchors.verticalCenter: parent.verticalCenter
                        keys: row.modelData.keys
                        recording: root.recording === row.modelData.id
                    }

                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            root.recording = row.modelData.id;
                            catcher.forceActiveFocus();
                        }
                    }
                }

                Dropdown {
                    id: action

                    x: 240
                    anchors.verticalCenter: parent.verticalCenter
                    fieldWidth: 210
                    value: row.modelData.action
                    options: Atrium.actions.map(a => ({ value: a, label: root.labels[a] ?? a }))
                    onPicked: v => Atrium.setShortcut(row.modelData.id, { action: v, arg: "" })
                }

                TextControl {
                    anchors.left: action.right
                    anchors.leftMargin: 8
                    anchors.right: remove.left
                    anchors.rightMargin: 8
                    anchors.verticalCenter: parent.verticalCenter
                    visible: row.hint.length > 0
                    placeholder: row.hint
                    value: row.modelData.arg ?? ""
                    onCommitted: v => Atrium.setShortcut(row.modelData.id, { arg: v })
                }

                MaterialIcon {
                    id: remove

                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    text: "remove_circle"
                    font.pointSize: Theme.font.size.larger
                    color: removeArea.containsMouse ? "#ffb4ab" : Theme.palette.m3OnSurfaceVariant

                    MouseArea {
                        id: removeArea

                        anchors.fill: parent
                        anchors.margins: -4
                        hoverEnabled: true
                        onClicked: Atrium.removeShortcut(row.modelData.id)
                    }
                }
            }
        }
    }
}
