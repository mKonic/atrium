pragma ComponentBehavior: Bound

import QtQuick
import shell.components
import shell.services
import Atrium

// Rules for what an app's own record can't say: windows matched by title,
// or by a pattern over app ids. Each is written in a sheet, as macOS adds
// things; the list says what each one does.
Rectangle {
    id: root

    width: parent?.width ?? 0
    height: column.implicitHeight + 32
    radius: 14
    color: Theme.palette.groupedBackground
    border.width: 1
    border.color: Theme.palette.separator

    function describe(r: var): string {
        const parts = [];
        if (r.secret)
            parts.push(`Opens in ${r.secret}`);
        else if (r.space)
            parts.push(`Opens in space ${r.space}`);
        if (r.follow && (r.secret || r.space))
            parts.push("goes there with it");
        if (r.fullscreen)
            parts.push("fullscreen");
        else if (r.maximized)
            parts.push("maximized");
        if (r.floating)
            parts.push("floats when tiling");
        if (r.keep_above)
            parts.push("kept above");
        if (r.sticky)
            parts.push("on every space");
        if (r.no_focus)
            parts.push("opens without focus");
        if (r.render_unfocused)
            parts.push("keeps drawing out of sight");
        const text = parts.join(", ");
        return text ? text.charAt(0).toUpperCase() + text.slice(1) : "Does nothing yet";
    }

    Column {
        id: column

        x: 16
        y: 16
        width: parent.width - 32
        spacing: 4

        SectionHeader {
            width: parent.width
            title: qsTr("Rules")
            subtitle: qsTr("Windows matched by title or an app id pattern. For a whole app, set where it opens under Apps.")

            PillButton {
                text: qsTr("Add")
                icon: "add"
                primary: true
                onClicked: sheet.edit(null)
            }
        }

        Item {
            width: 1
            height: 8
        }

        StyledText {
            visible: Atrium.rules.length === 0
            topPadding: 6
            bottomPadding: 6
            text: qsTr("No rules.")
            color: Theme.palette.secondaryLabel
        }

        Repeater {
            model: Atrium.rules

            Item {
                id: rule

                required property var modelData
                required property int index

                width: column.width
                height: 56

                Rectangle {
                    visible: rule.index > 0
                    width: parent.width
                    height: 1
                    color: Theme.palette.separator
                }

                Column {
                    anchors.left: parent.left
                    anchors.right: buttons.left
                    anchors.rightMargin: 12
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 2

                    StyledText {
                        width: parent.width
                        elide: Text.ElideRight
                        text: [rule.modelData.app_pattern ? qsTr("App id “%1”").arg(rule.modelData.app_pattern) : "",
                               rule.modelData.title_pattern ? `title “${rule.modelData.title_pattern}”` : ""]
                              .filter(s => s).join(", ")
                    }

                    StyledText {
                        width: parent.width
                        elide: Text.ElideRight
                        text: root.describe(rule.modelData)
                        font.pointSize: Theme.font.size.small
                        color: Theme.palette.secondaryLabel
                    }
                }

                Row {
                    id: buttons

                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 8

                    PillButton {
                        text: qsTr("Edit")
                        onClicked: sheet.edit(rule.modelData)
                    }

                    PillButton {
                        text: qsTr("Remove")
                        onClicked: Atrium.removeRule(rule.modelData.id)
                    }
                }
            }
        }
    }

    Sheet {
        id: sheet

        property var rule: null  // the one being edited; null: a new one
        property var draft: ({})

        function edit(r: var): void {
            rule = r;
            draft = r ? Object.assign({}, r) : {};
            appField.text = r?.app_pattern ?? "";
            titleField.text = r?.title_pattern ?? "";
            open();
        }

        width: 600
        title: rule ? qsTr("Edit Rule") : qsTr("New Rule")
        action: rule ? "Save" : "Add"
        ready: appField.text.trim() !== "" || titleField.text.trim() !== ""
        onOpened: appField.focusField()
        onSubmitted: {
            const fields = Object.assign({}, draft, {
                app_pattern: appField.text.trim(),
                title_pattern: titleField.text.trim()
            });
            delete fields.id;
            busy = true;
            if (rule)
                Atrium.setRule(rule.id, fields);
            else
                Atrium.addRule(fields);
        }

        // The answer: saved (the rules change) or refused (a bad pattern).
        Connections {
            target: Atrium
            enabled: sheet.busy

            function onRulesChanged(): void {
                sheet.busy = false;
                sheet.close();
            }

            function onRefused(why: string): void {
                sheet.busy = false;
                sheet.error = why;
            }
        }

        StyledText {
            width: parent.width
            wrapMode: Text.WordWrap
            text: qsTr("Patterns are regular expressions, matched anywhere and without regard to case: “firefox”, “^steam_app_”.")
            font.pointSize: Theme.font.size.smaller
            color: Theme.palette.secondaryLabel
        }

        Row {
            spacing: 12

            StyledText {
                anchors.verticalCenter: parent.verticalCenter
                width: 150
                text: qsTr("App id")
                font.pointSize: Theme.font.size.small
                color: Theme.palette.secondaryLabel
            }

            Field {
                id: appField

                implicitWidth: 360
                placeholder: qsTr("Any")
                onAccepted: if (sheet.ready) sheet.submitted()
            }
        }

        Row {
            spacing: 12

            StyledText {
                anchors.verticalCenter: parent.verticalCenter
                width: 150
                text: qsTr("Title")
                font.pointSize: Theme.font.size.small
                color: Theme.palette.secondaryLabel
            }

            Field {
                id: titleField

                implicitWidth: 360
                placeholder: qsTr("Any")
                onAccepted: if (sheet.ready) sheet.submitted()
            }
        }

        WindowOptions {
            record: sheet.draft
            onChanged: fields => sheet.draft = Object.assign({}, sheet.draft, fields)
        }
    }
}
