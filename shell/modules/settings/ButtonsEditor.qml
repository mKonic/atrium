pragma ComponentBehavior: Bound

import QtQuick
import Atrium.Shell
import shell.components
import shell.services
import Atrium

// A mouse's other buttons, as KWin's mouse settings rebind them: Add Button
// waits for a press of the one to change, then it presses keys (recorded as
// a shortcut is), acts as another button, or does nothing.
Column {
    id: root

    property var value: []
    signal committed(var value)

    property int recording: 0       // the button whose keys are being recorded
    property int pending: 0         // a button just added, shown until its action is picked
    property bool listening: false  // waiting for the button to add
    property string note: ""

    spacing: 4
    onRecordingChanged: if (recording !== 0) catcher.forceActiveFocus()

    // The keys reach this window, not atrium's shortcuts, while recording.
    ShortcutInhibitor {
        window: root.Window.window
        enabled: root.recording !== 0
    }

    Item {
        id: catcher

        focus: root.recording !== 0
        Keys.onPressed: event => {
            event.accepted = true;
            if (event.key === Qt.Key_Escape && event.modifiers === Qt.NoModifier) {
                root.recording = 0;
                root.pending = 0;
                return;
            }
            const keys = SettingsPages.chord(event.key, event.modifiers, "none");
            if (!keys)
                return;  // a modifier on its own: wait for the key
            root.committed(SettingsPages.remapWith(root.value, root.recording, "keys", keys));
            root.recording = 0;
            root.pending = 0;
        }
    }

    Repeater {
        model: root.pending ? SettingsPages.remapWith(root.value, root.pending, "keys") : root.value

        Item {
            id: row

            required property var modelData
            readonly property int button: modelData.button
            readonly property string action: SettingsPages.remapAction(modelData)

            width: root.width
            height: 40

            StyledText {
                anchors.verticalCenter: parent.verticalCenter
                width: 110
                text: SettingsPages.buttonName(row.button)
                font.weight: Font.DemiBold
                elide: Text.ElideRight
            }

            Dropdown {
                id: actionChoice

                x: 120
                anchors.verticalCenter: parent.verticalCenter
                fieldWidth: 150
                options: SettingsPages.remapActions()
                value: row.action
                onPicked: v => {
                    if (v === "keys") {
                        root.recording = row.button;  // committed once keys are pressed
                    } else {
                        root.recording = 0;
                        root.pending = 0;
                        root.committed(SettingsPages.remapWith(root.value, row.button, v));
                    }
                }
            }

            KeyCaps {
                anchors.left: actionChoice.right
                anchors.leftMargin: 12
                anchors.verticalCenter: parent.verticalCenter
                // Keys, or the modifiers held with another button.
                visible: keys !== "" || recording
                keys: row.modelData.keys ?? row.modelData.modifiers ?? ""
                recording: root.recording === row.button

                TapHandler {
                    onTapped: root.recording = row.button
                }
            }

            PillButton {
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("Remove")
                onClicked: {
                    root.recording = 0;
                    root.pending = 0;
                    root.committed(SettingsPages.remapWithout(root.value, row.button));
                }
            }
        }
    }

    // Waiting for the button: a press here, with any button, names it.
    Rectangle {
        visible: root.listening
        width: root.width
        height: 64
        radius: 10
        color: Theme.palette.tertiaryFill
        border.width: 1
        border.color: Theme.palette.focusRing

        StyledText {
            anchors.centerIn: parent
            text: qsTr("Press the mouse button to change here")
            color: Theme.palette.secondaryLabel
        }

        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.AllButtons
            onPressed: mouse => {
                const code = SettingsPages.evdevButton(mouse.button);
                root.listening = false;
                if (code === 0)
                    return;
                if (mouse.button === Qt.LeftButton) {
                    root.note = "The left button stays as it is.";
                    return;
                }
                root.note = "";
                // What it does next: keys, unless another action is picked.
                root.pending = code;
                root.recording = code;
            }
        }
    }

    Row {
        spacing: 8
        topPadding: 4

        PillButton {
            text: root.listening ? qsTr("Cancel") : qsTr("Add Button…")
            onClicked: {
                root.note = "";
                root.recording = 0;
                root.pending = 0;
                root.listening = !root.listening;
            }
        }

        StyledText {
            anchors.verticalCenter: parent.verticalCenter
            visible: root.recording !== 0
            text: qsTr("Press the keys, or Escape to keep it as it is")
            font.pointSize: Theme.font.size.small
            color: Theme.palette.secondaryLabel
        }

        StyledText {
            anchors.verticalCenter: parent.verticalCenter
            text: root.note
            font.pointSize: Theme.font.size.small
            color: Theme.palette.secondaryLabel
        }
    }
}
