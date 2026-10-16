pragma ComponentBehavior: Bound

import QtQuick
import shell.components
import shell.modules.settings
import shell.services

// A question over the dialog, as GTK's popovers and message dialogs ask:
// a name (New Folder, Rename) checked as it's typed, a yes-or-no (Replace,
// Delete), or a message. `ask({ title, detail, field, text, select,
// check(text) → { error, warning }, accept, danger, cancel, run(text) })`.
Item {
    id: root

    property var q: null
    readonly property bool open: q !== null
    property string error
    property string warning

    function ask(question: var): void {
        q = question;
        field.text = question.text ?? "";
        recheck();
        if (question.field) {
            field.focusField();
            field.select(0, question.select ?? field.text.length);
        } else {
            forceActiveFocus();
        }
    }

    function close(): void {
        q = null;
        error = "";
        warning = "";
    }

    function recheck(): void {
        const r = q?.check ? q.check(field.text) : {};
        error = r.error ?? "";
        warning = r.warning ?? "";
    }

    function go(): void {
        if (!q || (q.field && (error !== "" || field.text === "")))
            return;
        const run = q.run;
        const text = field.text;
        close();
        run?.(text);
    }

    anchors.fill: parent
    visible: open
    Keys.onEscapePressed: close()
    Keys.onReturnPressed: go()
    Keys.onEnterPressed: go()

    Rectangle {
        anchors.fill: parent
        color: Theme.palette.scrim

        MouseArea {
            anchors.fill: parent
        }
    }

    Rectangle {
        anchors.centerIn: parent
        width: 380
        height: column.implicitHeight + 40
        radius: 14
        color: Theme.palette.windowBackground
        border.width: 1
        border.color: Theme.palette.separator

        Column {
            id: column

            anchors.centerIn: parent
            width: parent.width - 40
            spacing: 12

            StyledText {
                width: parent.width
                wrapMode: Text.Wrap
                text: root.q?.title ?? ""
                font.weight: Font.DemiBold
            }

            StyledText {
                visible: text !== ""
                width: parent.width
                wrapMode: Text.Wrap
                text: root.q?.detail ?? ""
                color: Theme.palette.secondaryLabel
                font.pointSize: Theme.font.size.small
            }

            Field {
                id: field

                visible: root.q?.field ?? false
                width: parent.width
                onTextChanged: root.recheck()
                onAccepted: root.go()
                Keys.onEscapePressed: root.close()
            }

            StyledText {
                visible: text !== ""
                width: parent.width
                wrapMode: Text.Wrap
                text: root.error !== "" ? root.error : root.warning
                color: root.error !== "" ? Theme.palette.red : Theme.palette.orange
                font.pointSize: Theme.font.size.smaller
            }

            Row {
                anchors.right: parent.right
                spacing: 10

                PillButton {
                    visible: root.q?.cancel !== false
                    text: qsTr("Cancel")
                    onClicked: root.close()
                }

                PillButton {
                    primary: true
                    enabled: !(root.q?.field ?? false) || (field.text !== "" && root.error === "")
                    opacity: enabled ? 1 : 0.5
                    text: root.q?.accept ?? qsTr("OK")
                    onClicked: root.go()
                }
            }
        }
    }
}
