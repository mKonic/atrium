pragma ComponentBehavior: Bound

import QtQuick
import shell.components
import shell.services
import Atrium

// GTK's GtkFileChooserEntry: a field that completes paths against the
// folder shown. The names that fit drop down under it (folders end in
// "/"); Tab puts in what they all share; opening or choosing a folder also
// completes inline (the rest selected, typed over). "~" is home, a
// relative path is under the folder shown.
Rectangle {
    id: root

    required property ChooserModel model
    property alias text: input.text
    property string placeholder
    property bool inlineCompletion: true
    readonly property alias editing: input.activeFocus
    signal accepted
    signal escaped

    function focusField(): void {
        input.forceActiveFocus();
    }

    function select(start: int, end: int): void {
        input.select(start, end);
    }

    function setText(t: string): void {
        input.text = t;
        input.cursorPosition = t.length;
    }

    // Completions follow what's typed in front of the cursor.
    property var matches: []
    property int pick: -1
    property bool completing: false  // inserting a completion isn't typing
    // Shown unless the one name that fits is what's typed already.
    readonly property bool listing: input.activeFocus && matches.length > 0 && !(matches.length === 1 && matches[0].text === input.text)

    function refresh(): void {
        const typed = input.text.slice(0, input.selectionStart === input.selectionEnd ? input.cursorPosition : input.selectionStart);
        matches = typed.length > 0 && input.activeFocus ? root.model.completions(typed) : [];
        pick = -1;
    }

    function insert(text: string): void {
        completing = true;
        input.text = text;
        input.cursorPosition = text.length;
        completing = false;
        refresh();
    }

    implicitWidth: 320
    implicitHeight: 32
    radius: 8
    color: Theme.palette.tertiaryFill
    border.width: 1
    border.color: input.activeFocus ? Theme.palette.focusRing : "transparent"

    TextInput {
        id: input

        selectionColor: Theme.palette.accent
        selectedTextColor: Theme.palette.labelOnAccent
        anchors.fill: parent
        anchors.leftMargin: 10
        anchors.rightMargin: 10
        verticalAlignment: TextInput.AlignVCenter
        color: Theme.palette.label
        font.family: Theme.font.sans
        font.pointSize: Theme.font.size.small
        clip: true
        selectByMouse: true
        onActiveFocusChanged: root.refresh()

        onTextEdited: {
            if (root.completing)
                return;
            root.refresh();
            // Inline: the shared rest after the cursor, selected to type over.
            if (root.inlineCompletion && cursorPosition === text.length && root.matches.length > 0) {
                const typed = text;
                const full = root.model.complete(typed);
                if (full.length > typed.length && full.startsWith(typed)) {
                    root.completing = true;
                    text = full;
                    select(typed.length, full.length);
                    root.completing = false;
                }
            }
        }
        onAccepted: {
            if (root.pick >= 0) {
                root.insert(root.matches[root.pick].text);
                return;
            }
            root.matches = [];
            root.accepted();
        }

        Keys.onPressed: event => {
            if (event.key === Qt.Key_Tab && !(event.modifiers & Qt.ControlModifier)) {
                // A selection (inline completion) is taken; else complete what's shared.
                if (selectionStart !== selectionEnd) {
                    deselect();
                    cursorPosition = text.length;
                    root.refresh();
                } else {
                    const full = root.model.complete(text);
                    if (full !== text)
                        root.insert(full);
                }
                event.accepted = true;
            } else if (event.key === Qt.Key_Down && root.listing) {
                root.pick = Math.min(root.matches.length - 1, root.pick + 1);
                event.accepted = true;
            } else if (event.key === Qt.Key_Up && root.listing) {
                root.pick = Math.max(-1, root.pick - 1);
                event.accepted = true;
            } else if (event.key === Qt.Key_Escape) {
                if (root.listing)
                    root.matches = [];
                else
                    root.escaped();
                event.accepted = true;
            }
        }

        Text {
            anchors.verticalCenter: parent.verticalCenter
            visible: !input.text && root.placeholder
            text: root.placeholder
            font: input.font
            color: Theme.palette.tertiaryLabel
        }
    }

    // The names that fit, under the field.
    Rectangle {
        visible: root.listing
        z: 100
        anchors.top: parent.bottom
        anchors.topMargin: 4
        width: parent.width
        height: Math.min(8, root.matches.length) * 28 + 8
        radius: Theme.rounding.normal
        color: Theme.material.regular
        border.width: 1
        border.color: Theme.palette.separator

        ListView {
            id: drop

            anchors.fill: parent
            anchors.margins: 4
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            model: root.matches
            currentIndex: root.pick
            onCurrentIndexChanged: positionViewAtIndex(Math.max(0, currentIndex), ListView.Contain)

            delegate: Rectangle {
                id: match

                required property var modelData
                required property int index

                width: drop.width
                height: 28
                radius: 6
                color: index === root.pick ? Theme.palette.accent : matchArea.containsMouse ? Theme.palette.tertiaryFill : "transparent"

                Row {
                    x: 8
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 8

                    MaterialIcon {
                        anchors.verticalCenter: parent.verticalCenter
                        text: match.modelData.isDir ? "folder" : "draft"
                        color: match.index === root.pick ? Theme.palette.labelOnAccent : Theme.palette.secondaryLabel
                    }

                    StyledText {
                        anchors.verticalCenter: parent.verticalCenter
                        text: match.modelData.name
                        font.pointSize: Theme.font.size.small
                        color: match.index === root.pick ? Theme.palette.labelOnAccent : Theme.palette.label
                    }
                }

                MouseArea {
                    id: matchArea

                    anchors.fill: parent
                    hoverEnabled: true
                    onClicked: {
                        root.insert(match.modelData.text);
                        input.forceActiveFocus();
                    }
                }
            }
        }
    }
}
