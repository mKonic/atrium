pragma ComponentBehavior: Bound

import QtCore
import QtQuick
import Atrium.Shell
import shell.components
import shell.services
import Atrium

// Settings, Launcher: what the palette (Super+Space) holds besides apps.
// Quicklinks, snippets, custom commands and window sizes are written in
// sheets; Search Items gives any entry an alias, a global shortcut, or a
// hide from search. One tab at a time, so the page stays short.
Column {
    id: root

    required property var window
    property string tab: "Quicklinks"
    // What the last import or export did.
    property string quicklinkNote: ""
    property bool quicklinkFailed: false
    property string itemQuery: ""
    // The key being given a shortcut ("" none).
    property string recording: ""
    readonly property var tabs: ["Quicklinks", "Snippets", "Commands", "Windows", "Search Items"]

    // "settings:Launcher/quicklinks" shows a tab; "…/quicklink" starts a new one.
    function show(sub: string): void {
        const tabFor = { quicklink: "Quicklinks", quicklinks: "Quicklinks", snippet: "Snippets", snippets: "Snippets",
                         command: "Commands", commands: "Commands", sizes: "Windows", items: "Search Items" };
        if (tabFor[sub])
            tab = tabFor[sub];
        if (sub === "quicklink")
            quicklinkSheet.edit(null);
        else if (sub === "snippet")
            snippetSheet.edit(null);
        else if (sub === "command")
            commandSheet.edit(null);
    }

    spacing: 16

    LauncherModel {
        id: lm

        entries: DesktopEntries
    }

    // Apps a quicklink can open with, by name.
    readonly property var apps: [{ value: "", label: qsTr("Default App") }].concat(
        DesktopEntries.applications.values.filter(e => !e.noDisplay)
            .map(e => ({ value: e.id, label: e.name }))
            .sort((a, b) => a.label.localeCompare(b.label)))

    ChoiceControl {
        anchors.horizontalCenter: parent.horizontalCenter
        choices: root.tabs
        value: root.tab
        onPicked: v => root.tab = v
    }

    // --- one card per tab --------------------------------------------------------------

    Rectangle {
        width: parent.width
        height: records.implicitHeight + 32
        radius: 14
        visible: root.tab !== "Search Items"
        color: Theme.palette.groupedBackground
        border.width: 1
        border.color: Theme.palette.separator

        Column {
            id: records

            readonly property string table: ({ "Quicklinks": "quicklinks", "Snippets": "snippets",
                                               "Commands": "commands", "Windows": "window_sizes" })[root.tab] ?? ""
            readonly property var list: table ? recordsOf.value : []

            x: 16
            y: 16
            width: parent.width - 32
            spacing: 4

            // Atrium.records() isn't a property; this re-reads it when its table changes.
            QtObject {
                id: recordsOf

                property var value: records.table ? Atrium.records(records.table) : []
            }

            Connections {
                target: Atrium

                function onRecordsChanged(table: string): void {
                    if (table === records.table)
                        recordsOf.value = Atrium.records(table);
                }
            }

            SectionHeader {
                width: parent.width
                title: root.tab === "Windows" ? qsTr("Window Sizes") : root.tab
                subtitle: ({
                    "Quicklinks": "Addresses, searches, files and folders opened from the palette. {argument} is asked for when it runs; {clipboard}, {date} and {time} are filled in.",
                    "Snippets": "Text typed where you are typing, from the palette or by typing its keyword (turn on keyword expansion above). {clipboard}, {date}, {time} and {argument} work here too.",
                    "Commands": "Shell commands you run from the palette. {argument} is asked for and passed quoted.",
                    "Windows": "Sizes of your own for the focused window, as a share of the screen, centered."
                })[root.tab] ?? ""

                // Tinycast's Import and Export Quicklinks: a JSON file of them.
                PillButton {
                    visible: root.tab === "Quicklinks"
                    text: qsTr("Import…")
                    onClicked: quicklinkImport.open()
                }

                PillButton {
                    visible: root.tab === "Quicklinks"
                    text: QuicklinkFiles.busy ? qsTr("Exporting…") : qsTr("Export…")
                    enabled: !QuicklinkFiles.busy && records.list.length > 0
                    onClicked: {
                        root.quicklinkNote = "";
                        QuicklinkFiles.exportAll();
                    }
                }

                PillButton {
                    text: qsTr("Add")
                    icon: "add"
                    primary: true
                    onClicked: {
                        if (root.tab === "Quicklinks")
                            quicklinkSheet.edit(null);
                        else if (root.tab === "Snippets")
                            snippetSheet.edit(null);
                        else if (root.tab === "Commands")
                            commandSheet.edit(null);
                        else
                            sizeSheet.edit(null);
                    }
                }
            }

            Item {
                width: 1
                height: 8
            }

            StyledText {
                width: parent.width
                visible: root.tab === "Quicklinks" && root.quicklinkNote !== ""
                text: root.quicklinkNote
                wrapMode: Text.WordWrap
                font.pointSize: Theme.font.size.small
                color: root.quicklinkFailed ? Theme.palette.red : Theme.palette.secondaryLabel
            }

            StyledText {
                visible: records.list.length === 0
                topPadding: 6
                bottomPadding: 6
                text: root.tab === "Windows" ? qsTr("No window sizes yet.") : qsTr("No %1 yet.").arg(root.tab.toLowerCase())
                color: Theme.palette.secondaryLabel
            }

            Repeater {
                model: records.list

                Item {
                    id: rec

                    required property var modelData
                    required property int index

                    width: records.width
                    height: 52

                    Rectangle {
                        visible: rec.index > 0
                        width: parent.width
                        height: 1
                        color: Theme.palette.separator
                    }

                    Column {
                        anchors.left: parent.left
                        anchors.right: recButtons.left
                        anchors.rightMargin: 12
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 2

                        StyledText {
                            width: parent.width
                            elide: Text.ElideRight
                            text: rec.modelData.name || qsTr("Untitled")
                        }

                        StyledText {
                            width: parent.width
                            elide: Text.ElideRight
                            text: root.tab === "Quicklinks" ? rec.modelData.url
                                : root.tab === "Snippets" ? (rec.modelData.keyword ? `“${rec.modelData.keyword}” · ` : "") + rec.modelData.text.split("\n")[0]
                                : root.tab === "Commands" ? rec.modelData.command.split("\n")[0]
                                : `${Math.round(rec.modelData.width * 100)}% × ${Math.round(rec.modelData.height * 100)}%`
                            font.pointSize: Theme.font.size.small
                            color: Theme.palette.secondaryLabel
                        }
                    }

                    Row {
                        id: recButtons

                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 8

                        PillButton {
                            text: qsTr("Edit")
                            onClicked: {
                                if (root.tab === "Quicklinks")
                                    quicklinkSheet.edit(rec.modelData);
                                else if (root.tab === "Snippets")
                                    snippetSheet.edit(rec.modelData);
                                else if (root.tab === "Commands")
                                    commandSheet.edit(rec.modelData);
                                else
                                    sizeSheet.edit(rec.modelData);
                            }
                        }

                        PillButton {
                            text: qsTr("Remove")
                            onClicked: Atrium.removeRecord(records.table, rec.modelData.id)
                        }
                    }
                }
            }
        }
    }

    // --- window layouts (on the Windows tab) ---------------------------------------------------

    Rectangle {
        width: parent.width
        height: layoutsColumn.implicitHeight + 32
        radius: 14
        visible: root.tab === "Windows"
        color: Theme.palette.groupedBackground
        border.width: 1
        border.color: Theme.palette.separator

        Column {
            id: layoutsColumn

            property var list: lm.windowLayouts()

            x: 16
            y: 16
            width: parent.width - 32
            spacing: 4

            Connections {
                target: Atrium

                function onRecordsChanged(table: string): void {
                    if (table === "layout_windows")
                        layoutsColumn.list = lm.windowLayouts();
                }
            }

            SectionHeader {
                width: parent.width
                title: qsTr("Window Layouts")
                subtitle: qsTr("Arrangements the palette puts back in one go. Save one with Save Window Layout in the palette: it takes the windows on the space you're on.")
            }

            Item {
                width: 1
                height: 8
            }

            StyledText {
                visible: layoutsColumn.list.length === 0
                topPadding: 6
                bottomPadding: 6
                text: qsTr("No window layouts yet.")
                color: Theme.palette.secondaryLabel
            }

            Repeater {
                model: layoutsColumn.list

                Item {
                    id: lay

                    required property var modelData
                    required property int index

                    width: layoutsColumn.width
                    height: 52

                    Rectangle {
                        visible: lay.index > 0
                        width: parent.width
                        height: 1
                        color: Theme.palette.separator
                    }

                    Column {
                        anchors.left: parent.left
                        anchors.right: layButtons.left
                        anchors.rightMargin: 12
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 2

                        StyledText {
                            text: lay.modelData.name
                        }

                        StyledText {
                            text: lay.modelData.windows === 1 ? qsTr("1 window") : qsTr("%1 windows").arg(lay.modelData.windows)
                            font.pointSize: Theme.font.size.small
                            color: Theme.palette.secondaryLabel
                        }
                    }

                    Row {
                        id: layButtons

                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 8

                        PillButton {
                            text: qsTr("Rename")
                            onClicked: layoutSheet.edit(lay.modelData.name)
                        }

                        PillButton {
                            text: qsTr("Remove")
                            onClicked: lm.removeLayout(lay.modelData.name)
                        }
                    }
                }
            }
        }
    }

    Sheet {
        id: layoutSheet

        property string name

        function edit(n: string): void {
            name = n;
            layoutName.text = n;
            open();
        }

        width: 520
        title: qsTr("Rename Window Layout")
        action: "Rename"
        ready: layoutName.text.trim() !== ""
        onOpened: layoutName.focusField()
        onSubmitted: {
            lm.renameLayout(name, layoutName.text.trim());
            close();
        }

        Field {
            id: layoutName

            implicitWidth: 460
            onAccepted: if (layoutSheet.ready) layoutSheet.submitted()
        }
    }

    // --- Search Items: alias, shortcut, hidden -----------------------------------------------

    Rectangle {
        width: parent.width
        height: items.implicitHeight + 32
        radius: 14
        visible: root.tab === "Search Items"
        color: Theme.palette.groupedBackground
        border.width: 1
        border.color: Theme.palette.separator

        // The keys reach this window, not atrium's own shortcuts, while recording.
        ShortcutInhibitor {
            window: root.window
            enabled: root.recording !== ""
        }

        Item {
            id: catcher

            focus: root.recording !== ""
            Keys.onPressed: event => {
                event.accepted = true;
                const row = items.list.find(i => i.key === root.recording);
                if (event.key === Qt.Key_Escape && event.modifiers === Qt.NoModifier) {
                    root.recording = "";
                    return;
                }
                // Backspace alone takes the shortcut away.
                if ((event.key === Qt.Key_Backspace || event.key === Qt.Key_Delete) && event.modifiers === Qt.NoModifier) {
                    if (row && row.shortcut >= 0)
                        Atrium.removeShortcut(row.shortcut);
                    root.recording = "";
                    return;
                }
                const keys = SettingsPages.chord(event.key, event.modifiers, Atrium.settings["shortcuts.modifier"] ?? "super");
                if (!keys)
                    return;  // a modifier on its own: wait for the key
                if (row && row.shortcut >= 0)
                    Atrium.setShortcut(row.shortcut, { keys: keys });
                else
                    Atrium.addShortcut({ keys: keys, action: "shell", arg: "run:" + root.recording });
                root.recording = "";
            }
        }

        Column {
            id: items

            property var list: []

            function refresh(): void {
                list = lm.items(root.itemQuery);
            }

            x: 16
            y: 16
            width: parent.width - 32
            spacing: 4

            Component.onCompleted: refresh()

            Connections {
                target: Atrium

                function onRecordsChanged(table: string): void {
                    if (table === "launcher_entries")
                        refreshLater.restart();
                }
                function onShortcutsChanged(): void {
                    refreshLater.restart();
                }
            }

            // The model hears the same changes; ask it once it has.
            Timer {
                id: refreshLater

                interval: 50
                onTriggered: items.refresh()
            }

            SectionHeader {
                width: parent.width
                title: qsTr("Search Items")
                subtitle: qsTr("An alias finds an entry by a word of your own. A shortcut runs it from anywhere; click to record, Backspace clears. Hidden entries leave search, and keep their shortcut.")
            }

            Field {
                width: parent.width
                placeholder: qsTr("Filter")
                onTextChanged: {
                    root.itemQuery = text;
                    items.refresh();
                }
            }

            Item {
                width: 1
                height: 4
            }

            Repeater {
                model: items.list

                Item {
                    id: item

                    required property var modelData
                    required property int index
                    readonly property bool newSection: index === 0 || items.list[index - 1].section !== modelData.section

                    width: items.width
                    height: 40 + (newSection ? 30 : 0)

                    StyledText {
                        visible: item.newSection
                        y: 10
                        text: item.modelData.section
                        font.pointSize: Theme.font.size.small
                        font.weight: Font.Medium
                        color: Theme.palette.secondaryLabel
                    }

                    Item {
                        y: item.newSection ? 30 : 0
                        width: parent.width
                        height: 40
                        opacity: item.modelData.hidden ? 0.5 : 1

                        Item {
                            id: art

                            anchors.left: parent.left
                            anchors.verticalCenter: parent.verticalCenter
                            width: 22
                            height: 22

                            IconImage {
                                anchors.fill: parent
                                visible: item.modelData.icon.length > 0
                                implicitSize: 22
                                source: item.modelData.icon.length === 0 ? "" : item.modelData.icon.startsWith("file:") ? item.modelData.icon : Shell.iconPath(item.modelData.icon, "application-x-executable")
                                asynchronous: true
                            }

                            Rectangle {
                                anchors.fill: parent
                                visible: item.modelData.icon.length === 0
                                radius: 6
                                color: item.modelData.color || Theme.palette.tertiaryFill

                                MaterialIcon {
                                    anchors.centerIn: parent
                                    text: item.modelData.glyph
                                    font.pointSize: 11
                                    color: item.modelData.color ? "white" : Theme.palette.label
                                }
                            }
                        }

                        StyledText {
                            anchors.left: art.right
                            anchors.leftMargin: 10
                            anchors.right: aliasField.left
                            anchors.rightMargin: 10
                            anchors.verticalCenter: parent.verticalCenter
                            elide: Text.ElideRight
                            text: item.modelData.title
                        }

                        Field {
                            id: aliasField

                            anchors.right: keysBox.left
                            anchors.rightMargin: 10
                            anchors.verticalCenter: parent.verticalCenter
                            implicitWidth: 120
                            placeholder: qsTr("Alias")
                            text: item.modelData.alias
                            // Stored as typed, trimmed when left.
                            onAccepted: lm.setAlias(item.modelData.key, text)
                            onActiveFocusChanged: if (!activeFocus && text.trim() !== item.modelData.alias) lm.setAlias(item.modelData.key, text)
                        }

                        Rectangle {
                            id: keysBox

                            anchors.right: hideSwitch.left
                            anchors.rightMargin: 10
                            anchors.verticalCenter: parent.verticalCenter
                            width: 150
                            height: 28
                            radius: 8
                            color: Theme.palette.tertiaryFill
                            border.width: root.recording === item.modelData.key ? 2 : 0
                            border.color: Theme.palette.focusRing

                            KeyCaps {
                                anchors.centerIn: parent
                                visible: item.modelData.keys.length > 0 || root.recording === item.modelData.key
                                keys: item.modelData.keys
                                recording: root.recording === item.modelData.key
                            }

                            StyledText {
                                anchors.centerIn: parent
                                visible: item.modelData.keys.length === 0 && root.recording !== item.modelData.key
                                text: qsTr("Record Shortcut")
                                font.pointSize: Theme.font.size.small
                                color: Theme.palette.tertiaryLabel
                            }

                            MouseArea {
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor
                                onClicked: {
                                    root.recording = item.modelData.key;
                                    catcher.forceActiveFocus();
                                }
                            }
                        }

                        // Shown in search: off hides it (only where this page can bring it back).
                        Switch {
                            name: qsTr("Show in search")
                            id: hideSwitch

                            anchors.right: parent.right
                            anchors.verticalCenter: parent.verticalCenter
                            visible: item.modelData.hideable
                            checked: !item.modelData.hidden
                            onToggled: lm.setHidden(item.modelData.key, !item.modelData.hidden)
                        }
                    }
                }
            }
        }
    }

    // --- the sheets ---------------------------------------------------------------------------

    // A row of a sheet: a label and its control.
    component FormRow: Row {
        property string label
        default property alias control: slot.data

        spacing: 12

        StyledText {
            anchors.verticalCenter: parent.verticalCenter
            width: 130
            text: parent.label
            font.pointSize: Theme.font.size.small
            color: Theme.palette.secondaryLabel
        }

        Item {
            id: slot

            width: childrenRect.width
            height: childrenRect.height
        }
    }

    // Several lines of text, as a Field is one.
    component TextBox: Rectangle {
        property alias text: edit.text
        property bool mono: false

        function focusField(): void {
            edit.forceActiveFocus();
        }

        implicitWidth: 400
        implicitHeight: 120
        radius: 8
        color: Theme.palette.tertiaryFill
        border.width: 1
        border.color: edit.activeFocus ? Theme.palette.focusRing : "transparent"

        Flickable {
            id: flick

            anchors.fill: parent
            anchors.margins: 8
            contentHeight: edit.implicitHeight
            clip: true
            boundsBehavior: Flickable.StopAtBounds

            TextEdit {
                id: edit

                width: flick.width
                wrapMode: TextEdit.Wrap
                color: Theme.palette.label
                font.family: parent.parent.mono ? Theme.font.mono : Theme.font.sans
                font.pointSize: Theme.font.size.small
                selectByMouse: true
                activeFocusOnTab: true
                selectionColor: Theme.palette.accent
                selectedTextColor: Theme.palette.labelOnAccent
                onCursorRectangleChanged: {
                    if (cursorRectangle.y < flick.contentY)
                        flick.contentY = cursorRectangle.y;
                    else if (cursorRectangle.y + cursorRectangle.height > flick.contentY + flick.height)
                        flick.contentY = cursorRectangle.y + cursorRectangle.height - flick.height;
                }
            }
        }
    }

    // Saves a record: added or changed, then the sheet closes once the list has it.
    component RecordSheet: Sheet {
        id: sheet

        property string table
        property var record: null  // the one being edited; null: a new one

        function save(fields: var): void {
            busy = true;
            if (record)
                Atrium.setRecord(table, record.id, fields);
            else
                Atrium.addRecord(table, fields);
        }

        width: 600
        action: record ? "Save" : "Add"

        Connections {
            target: Atrium
            enabled: sheet.busy

            function onRecordsChanged(table: string): void {
                if (table !== sheet.table)
                    return;
                sheet.busy = false;
                sheet.close();
            }

            function onRefused(why: string): void {
                sheet.busy = false;
                sheet.error = why;
            }
        }
    }

    RecordSheet {
        id: quicklinkSheet

        property string app: ""
        property bool inRoot: true

        function edit(r: var): void {
            record = r;
            qlName.text = r?.name ?? "";
            qlUrl.text = r?.url ?? "";
            app = r?.app ?? "";
            inRoot = r?.root ?? true;
            open();
        }

        table: "quicklinks"
        title: record ? qsTr("Edit Quicklink") : qsTr("New Quicklink")
        ready: qlName.text.trim() !== "" && qlUrl.text.trim() !== ""
        onOpened: qlName.focusField()
        onSubmitted: save({ name: qlName.text.trim(), url: qlUrl.text.trim(), app: app, root: inRoot })

        FormRow {
            label: qsTr("Name")

            Field {
                id: qlName

                implicitWidth: 400
                placeholder: qsTr("Search GitHub")
            }
        }

        FormRow {
            label: qsTr("Address or path")

            Field {
                id: qlUrl

                implicitWidth: 400
                placeholder: "https://github.com/search?q={argument}"
                onAccepted: if (quicklinkSheet.ready) quicklinkSheet.submitted()
            }
        }

        FormRow {
            label: qsTr("Open with")

            Dropdown {
                fieldWidth: 400
                options: root.apps
                value: quicklinkSheet.app
                onPicked: v => quicklinkSheet.app = v
            }
        }

        FormRow {
            label: qsTr("In root search")

            Switch {
                name: qsTr("In root search")
                checked: quicklinkSheet.inRoot
                onToggled: quicklinkSheet.inRoot = !quicklinkSheet.inRoot
            }
        }
    }

    RecordSheet {
        id: snippetSheet

        function edit(r: var): void {
            record = r;
            snName.text = r?.name ?? "";
            snKeyword.text = r?.keyword ?? "";
            snText.text = r?.text ?? "";
            open();
        }

        table: "snippets"
        title: record ? qsTr("Edit Snippet") : qsTr("New Snippet")
        ready: snName.text.trim() !== "" && snText.text !== ""
        onOpened: snName.focusField()
        onSubmitted: save({ name: snName.text.trim(), keyword: snKeyword.text.trim(), text: snText.text })

        FormRow {
            label: qsTr("Name")

            Field {
                id: snName

                implicitWidth: 400
                placeholder: qsTr("Email signature")
            }
        }

        FormRow {
            label: qsTr("Keyword")

            Field {
                id: snKeyword

                implicitWidth: 400
                placeholder: qsTr("Optional, like ;sig")
            }
        }

        FormRow {
            label: qsTr("Text")

            TextBox {
                id: snText

                implicitHeight: 160
            }
        }
    }

    RecordSheet {
        id: commandSheet

        property bool output: false
        property bool terminal: false
        property bool confirm: false

        function edit(r: var): void {
            record = r;
            cmdName.text = r?.name ?? "";
            cmdText.text = r?.command ?? "";
            cmdDir.text = r?.directory ?? "";
            cmdIcon.text = r?.icon ?? "";
            output = r?.output ?? false;
            terminal = r?.terminal ?? false;
            confirm = r?.confirm ?? false;
            open();
        }

        table: "commands"
        title: record ? qsTr("Edit Command") : qsTr("New Command")
        ready: cmdName.text.trim() !== "" && cmdText.text.trim() !== ""
        onOpened: cmdName.focusField()
        onSubmitted: save({ name: cmdName.text.trim(), command: cmdText.text, directory: cmdDir.text.trim(),
                            icon: cmdIcon.text.trim(), output: output, terminal: terminal, confirm: confirm })

        FormRow {
            label: qsTr("Name")

            Field {
                id: cmdName

                implicitWidth: 400
                placeholder: qsTr("Update mirrors")
            }
        }

        FormRow {
            label: qsTr("Command")

            TextBox {
                id: cmdText

                mono: true
                implicitHeight: 100
            }
        }

        FormRow {
            label: qsTr("Run in")

            Field {
                id: cmdDir

                implicitWidth: 400
                placeholder: qsTr("Home folder")
            }
        }

        FormRow {
            label: qsTr("Icon")

            Field {
                id: cmdIcon

                implicitWidth: 400
                placeholder: qsTr("A Material Symbols name, like terminal")
            }
        }

        FormRow {
            label: qsTr("Show output")

            Switch {
                name: qsTr("Show output")
                checked: commandSheet.output
                onToggled: {
                    commandSheet.output = !commandSheet.output;
                    if (commandSheet.output)
                        commandSheet.terminal = false;
                }
            }
        }

        FormRow {
            label: qsTr("Run in terminal")

            Switch {
                name: qsTr("Run in terminal")
                checked: commandSheet.terminal
                onToggled: {
                    commandSheet.terminal = !commandSheet.terminal;
                    if (commandSheet.terminal)
                        commandSheet.output = false;
                }
            }
        }

        FormRow {
            label: qsTr("Ask first")

            Switch {
                name: qsTr("Ask first")
                checked: commandSheet.confirm
                onToggled: commandSheet.confirm = !commandSheet.confirm
            }
        }
    }

    RecordSheet {
        id: sizeSheet

        property real w: 0.6
        property real h: 0.6

        function edit(r: var): void {
            record = r;
            sizeName.text = r?.name ?? "";
            w = r?.width ?? 0.6;
            h = r?.height ?? 0.6;
            open();
        }

        table: "window_sizes"
        title: record ? qsTr("Edit Window Size") : qsTr("New Window Size")
        ready: sizeName.text.trim() !== ""
        onOpened: sizeName.focusField()
        onSubmitted: save({ name: sizeName.text.trim(), width: w, height: h })

        FormRow {
            label: qsTr("Name")

            Field {
                id: sizeName

                implicitWidth: 400
                placeholder: qsTr("Reading")
            }
        }

        FormRow {
            label: qsTr("Width")

            NumberControl {
                width: 400
                min: 10
                max: 100
                value: Math.round(sizeSheet.w * 100)
                onCommitted: v => sizeSheet.w = v / 100
            }
        }

        FormRow {
            label: qsTr("Height")

            NumberControl {
                width: 400
                min: 10
                max: 100
                value: Math.round(sizeSheet.h * 100)
                onCommitted: v => sizeSheet.h = v / 100
            }
        }
    }

    FilePicker {
        id: quicklinkImport

        title: qsTr("Import Quicklinks")
        folder: StandardPaths.writableLocation(StandardPaths.DownloadLocation)
        nameFilter: qsTr("Quicklinks (*.json)")
        onPicked: file => {
            root.quicklinkNote = "";
            QuicklinkFiles.importFile(file);
        }
    }

    Connections {
        target: QuicklinkFiles

        function onFinished(note: string, failed: bool): void {
            root.quicklinkNote = note;
            root.quicklinkFailed = failed;
        }
    }
}
