pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Effects
import Atrium.Shell
import shell.components
import shell.services
import Atrium

// The palette (Super+Space), after Tinycast: find and open apps, Settings
// pages, windows, quicklinks, snippets, your own commands, system actions and
// window commands; do sums; Ctrl+K for what else a row can do. Clipboard
// History (Super+V) and Emoji (Super+Period) are screens of it. The logic is
// LauncherModel's; this is its window.
PanelWindow {
    id: launcher

    readonly property bool listScreen: ["root", "windows", "snippets", "quicklinks", "calculator"].includes(lm.screen)
    // What the arguments beside the field hold, in the selected row's order.
    property var args: []
    property bool menuOpen: false
    property int menuCurrent: 0
    property var menuActions: []
    // Whose menu: the row's actions (a list screen's, or another screen's
    // own), or a screen's type filter.
    property string menuSource: "actions"
    property var confirming: null  // { title, detail, glyph, token } or, a screen's own, { ..., run }

    // Confirmed: the screen's own action, or the model's.
    function confirmNow(): void {
        const c = launcher.confirming;
        launcher.confirming = null;
        if (c.run)
            c.run();
        else
            lm.confirm(c.token);
    }

    readonly property var placeholders: ({
            "root": "Search for apps and commands…",
            "clipboard": "Search clipboard…",
            "emoji": "Search emoji…",
            "windows": "Search windows…",
            "snippets": "Search snippets…",
            "quicklinks": "Search quicklinks…",
            "files": "Search files…",
            "calculator": "Search calculations…",
            "notes": "Search notes…",
            "output": ""
        })
    readonly property var screenTitles: ({
            "clipboard": "Clipboard History",
            "emoji": "Emoji & Symbols",
            "windows": "Switch Windows",
            "snippets": "Snippets",
            "quicklinks": "Quicklinks",
            "files": "Files",
            "calculator": "Calculator History",
            "notes": "Notes",
            "output": ""
        })

    LauncherModel {
        id: lm

        entries: DesktopEntries

        onCloseRequested: launcher.close()
        onFeedback: (glyph, text, noop) => pill.show(glyph, text, noop)
        onConfirmRequested: (title, detail, glyph, token) => {
            launcher.confirming = { title, detail, glyph, token };
            if (!launcher.visible)
                launcher.show();
        }
        onOpenRequested: query => launcher.open("root", query)
        onCurrentChanged: launcher.args = []
        onQueryChanged: if (input.text !== lm.query) input.text = lm.query
        onScreenChanged: launcher.menuOpen = false
    }

    visible: false
    screen: Shell.screen(Atrium.focusedOutput?.name)
    anchors {
        top: true
        bottom: true
        left: true
        right: true
    }
    exclusiveZone: -1
    color: "transparent"
    WlrLayershell.layer: WlrLayer.Overlay
    WlrLayershell.keyboardFocus: visible ? WlrKeyboardFocus.Exclusive : WlrKeyboardFocus.None
    WlrLayershell.namespace: "atrium-launcher"

    function open(screenName: string, query: string): void {
        confirming = null;
        menuOpen = false;
        lm.open(screenName, query);
        input.text = lm.query;
        show();
    }

    function show(): void {
        visible = true;
        input.forceActiveFocus();
        shown.restart();
    }

    function close(): void {
        visible = false;
        menuOpen = false;
        confirming = null;
    }

    function activate(row: int): void {
        // A required argument still empty takes the keyboard instead.
        const wanted = lm.arguments;
        for (let i = 0; i < wanted.length; ++i)
            if (!wanted[i].optional && !(args[i] ?? "").length) {
                argFields.itemAt(i)?.focusField();
                return;
            }
        lm.activate(row, args);
    }

    function menuItems(query: string): var {
        if (menuSource === "filter") {
            const q = query.toLowerCase();
            return (body.item?.filters ?? []).filter(f => f.label.toLowerCase().includes(q)).map(f => ({
                id: f.value, title: f.label, glyph: f.value === body.item.filter ? "check" : "", keys: "", group: false
            }));
        }
        if (listScreen)
            return lm.count > 0 ? lm.actions(-1, query) : [];
        return body.item?.actions ? body.item.actions(query) : [];
    }

    function openMenu(source: string): void {
        menuSource = source;
        menuSearch.text = "";
        menuActions = menuItems("");
        if (menuActions.length === 0)
            return;
        // A pop-up opens on what it already holds.
        menuCurrent = Math.max(0, menuActions.findIndex(a => a.glyph === "check"));
        menuOpen = true;
        menuSearch.forceActiveFocus();
    }

    function closeMenu(): void {
        menuOpen = false;
        input.forceActiveFocus();
    }

    function runMenu(index: int): void {
        const a = menuActions[index];
        closeMenu();
        if (!a)
            return;
        if (menuSource === "filter")
            body.item.setFilter(a.id);
        else if (listScreen)
            lm.runAction(-1, a.id);
        else
            body.item.runAction(a.id);
    }

    Connections {
        target: Atrium

        function onShellAction(name: string): void {
            if (name === "launcher")
                launcher.visible && lm.screen === "root" ? launcher.close() : launcher.open("root", "");
            else if (name === "clipboard" || name === "emoji")
                launcher.visible && lm.screen === name ? launcher.close() : launcher.open(name, "");
            else if (name.startsWith("run:"))
                lm.run(name.slice(4));
        }

        function onSnippetTyped(snippet: int, before: int): void {
            lm.expandSnippet(snippet, before);
        }
    }

    // A click beside the panel puts it away.
    MouseArea {
        anchors.fill: parent
        onClicked: launcher.close()
    }

    Rectangle {
        id: panel

        anchors.horizontalCenter: parent.horizontalCenter
        y: Math.round(launcher.height * 0.18)
        width: Math.min(760, launcher.width - 64)
        height: Math.min(480, launcher.height - y - 48)
        radius: 22
        color: Theme.material.regular

        Glass {}

        border.width: Theme.lens ? 0 : 1
        border.color: Theme.palette.separator

        layer.enabled: true
        layer.effect: MultiEffect {
            shadowEnabled: !Theme.lens
            shadowColor: Theme.palette.shadow
            shadowBlur: 1
            shadowVerticalOffset: 8
        }

        // Opens with a small settle.
        ParallelAnimation {
            id: shown

            Anim {
                target: panel
                property: "scale"
                from: 0.97
                to: 1
                duration: Theme.anim.small
                easing.bezierCurve: Theme.anim.emphasizedDecel
            }
            Anim {
                target: panel
                property: "opacity"
                from: 0
                to: 1
                duration: Theme.anim.small
            }
        }

        MouseArea {
            anchors.fill: parent  // clicks on the panel stay on it
            onClicked: if (launcher.menuOpen) launcher.closeMenu()
        }

        // --- the field ---------------------------------------------------------------------

        Item {
            id: header

            width: parent.width
            height: 58

            // Off the root, the icon slot steps back (or closes a screen opened on its own).
            Rectangle {
                id: back

                anchors.left: parent.left
                anchors.leftMargin: 14
                anchors.verticalCenter: parent.verticalCenter
                width: 30
                height: 30
                radius: 8
                color: backArea.containsMouse && lm.screen !== "root" ? Theme.palette.tertiaryFill : "transparent"

                MaterialIcon {
                    anchors.centerIn: parent
                    text: lm.screen === "root" ? "search" : "arrow_back"
                    font.pointSize: 16
                    color: backArea.containsMouse ? Theme.palette.label : Theme.palette.secondaryLabel
                }

                MouseArea {
                    id: backArea

                    anchors.fill: parent
                    hoverEnabled: true
                    enabled: lm.screen !== "root"
                    onClicked: if (!lm.backspace()) launcher.close()
                }
            }

            TextInput {
                id: input

                anchors.left: back.right
                anchors.leftMargin: 10
                anchors.verticalCenter: parent.verticalCenter
                // Sized to what's typed while arguments follow it, as Raycast does.
                width: argRow.visible ? Math.min(Math.max(contentWidth + 4, 40), header.width * 0.45)
                                      : header.width - x - 20 - (filterPill.visible ? filterPill.width + 12 : 0)
                color: Theme.palette.label
                font.family: Theme.font.sans
                font.pointSize: 16
                selectionColor: Theme.palette.accent
                selectedTextColor: Theme.palette.labelOnAccent
                clip: true
                readOnly: lm.screen === "output"
                onTextChanged: if (lm.query !== text) lm.query = text

                // Ctrl held a moment shows the favorites' numbers.
                Keys.onReleased: event => {
                    if (event.key === Qt.Key_Control) {
                        ctrlHold.stop();
                        rootList.ctrlHeld = false;
                    }
                }
                Keys.onPressed: event => {
                    const ctrl = event.modifiers & Qt.ControlModifier;
                    if (event.key === Qt.Key_Control) {
                        ctrlHold.restart();
                        return;
                    }
                    ctrlHold.stop();
                    rootList.ctrlHeld = false;
                    event.accepted = true;
                    if (launcher.confirming) {
                        if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
                            launcher.confirmNow();
                        } else if (event.key === Qt.Key_Escape) {
                            launcher.confirming = null;
                        }
                        return;
                    }
                    if (event.key === Qt.Key_Escape) {
                        if (ctrl)
                            launcher.open("root", "");
                        else if (!lm.escape())
                            launcher.close();
                        return;
                    }
                    if (event.key === Qt.Key_Backspace && input.text.length === 0 && !ctrl) {
                        lm.backspace();
                        return;
                    }
                    if (ctrl && event.key === Qt.Key_K) {
                        launcher.openMenu("actions");
                        return;
                    }
                    if (event.key === Qt.Key_Tab && launcher.listScreen && lm.arguments.length > 0) {
                        argFields.itemAt(0)?.focusField();
                        return;
                    }
                    if (event.key === Qt.Key_Tab && (lm.screen === "root" || lm.screen === "clipboard")) {
                        lm.tab();
                        return;
                    }
                    if (body.item && body.item.handleKey(event))
                        return;
                    if (launcher.listScreen && (event.key === Qt.Key_Return || event.key === Qt.Key_Enter)) {
                        launcher.activate(-1);
                        return;
                    }
                    event.accepted = false;
                }

                StyledText {
                    anchors.fill: parent
                    visible: input.text.length === 0 && input.preeditText.length === 0 && !argRow.visible
                    text: lm.screen === "output" ? lm.outputTitle : (launcher.placeholders[lm.screen] ?? "")
                    elide: Text.ElideRight
                    font.pointSize: 16
                    color: lm.screen === "output" ? Theme.palette.label : Theme.palette.tertiaryLabel
                }
            }

            // A screen's type filter (Search Files), as a pop-up at the header's end.
            Rectangle {
                id: filterPill

                readonly property string label: (body.item?.filters ?? []).find(f => f.value === body.item?.filter)?.label ?? ""

                anchors.right: parent.right
                anchors.rightMargin: 16
                anchors.verticalCenter: parent.verticalCenter
                visible: label.length > 0
                width: filterRow.implicitWidth + 20
                height: 30
                radius: 8
                color: filterArea.containsMouse || (launcher.menuOpen && launcher.menuSource === "filter")
                       ? Theme.palette.tertiaryFill : Theme.palette.quaternaryFill

                Row {
                    id: filterRow

                    anchors.centerIn: parent
                    spacing: 4

                    StyledText {
                        anchors.verticalCenter: parent.verticalCenter
                        text: filterPill.label
                        font.pointSize: Theme.font.size.smaller
                    }

                    MaterialIcon {
                        anchors.verticalCenter: parent.verticalCenter
                        text: "expand_more"
                        color: Theme.palette.secondaryLabel
                    }
                }

                MouseArea {
                    id: filterArea

                    anchors.fill: parent
                    hoverEnabled: true
                    onClicked: launcher.menuOpen ? launcher.closeMenu() : launcher.openMenu("filter")
                }
            }

            Timer {
                id: ctrlHold

                interval: 400
                onTriggered: rootList.ctrlHeld = true
            }

            // The selected row's arguments, typed right after the query.
            Row {
                id: argRow

                anchors.left: input.right
                anchors.leftMargin: 10
                anchors.verticalCenter: parent.verticalCenter
                spacing: 8
                visible: launcher.listScreen && lm.arguments.length > 0

                Repeater {
                    id: argFields

                    model: launcher.listScreen ? lm.arguments : []

                    Rectangle {
                        id: argBox

                        required property var modelData
                        required property int index

                        function focusField(): void {
                            argInput.forceActiveFocus();
                        }

                        width: Math.max(90, argInput.contentWidth + 24)
                        height: 30
                        radius: 8
                        color: Theme.palette.quaternaryFill
                        border.width: argInput.activeFocus ? 2 : 1
                        border.color: argInput.activeFocus ? Theme.palette.focusRing : Theme.palette.separator

                        TextInput {
                            id: argInput

                            anchors.fill: parent
                            anchors.leftMargin: 10
                            anchors.rightMargin: 10
                            verticalAlignment: TextInput.AlignVCenter
                            color: Theme.palette.label
                            font.family: Theme.font.sans
                            font.pointSize: Theme.font.size.normal
                            selectionColor: Theme.palette.accent
                            selectedTextColor: Theme.palette.labelOnAccent
                            clip: true
                            onTextChanged: {
                                const a = launcher.args.slice();
                                a[argBox.index] = text;
                                launcher.args = a;
                            }

                            Keys.onPressed: event => {
                                event.accepted = true;
                                if (event.key === Qt.Key_Tab && argBox.index + 1 < argFields.count)
                                    argFields.itemAt(argBox.index + 1).focusField();
                                else if (event.key === Qt.Key_Tab || event.key === Qt.Key_Escape
                                         || event.key === Qt.Key_Backtab)
                                    input.forceActiveFocus();
                                else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter)
                                    launcher.activate(-1);
                                else
                                    event.accepted = false;
                            }

                            StyledText {
                                anchors.fill: parent
                                verticalAlignment: Text.AlignVCenter
                                visible: argInput.text.length === 0 && argInput.preeditText.length === 0
                                text: argBox.modelData.name + (argBox.modelData.optional ? " (optional)" : "")
                                color: Theme.palette.tertiaryLabel
                            }
                        }
                    }
                }
            }
        }

        Rectangle {
            id: rule

            anchors.top: header.bottom
            width: parent.width
            height: 1
            color: Theme.palette.separator
        }

        // --- the screen --------------------------------------------------------------------

        RootList {
            id: rootList

            anchors.top: rule.bottom
            anchors.bottom: footerRule.top
            width: parent.width
            visible: launcher.listScreen
            launcherModel: lm
            onActivated: row => {
                lm.current = row;
                launcher.activate(row);
            }
            // handleKey is reached through `body` below while it is the screen.
        }

        Loader {
            id: body

            anchors.top: rule.bottom
            anchors.bottom: footerRule.top
            width: parent.width
            // The list screens share the one RootList; the rest load their own.
            sourceComponent: launcher.listScreen ? listProxy
                           : lm.screen === "clipboard" ? clipboardScreen
                           : lm.screen === "emoji" ? emojiScreen
                           : lm.screen === "output" ? outputScreen
                           : lm.screen === "files" ? filesScreen
                           : lm.screen === "notes" ? notesScreen
                           : notYet
        }

        Component {
            id: listProxy

            Item {
                readonly property var hints: []
                function handleKey(event: var): bool {
                    return rootList.handleKey(event);
                }
            }
        }

        Component {
            id: clipboardScreen

            ClipboardScreen {
                onConfirm: request => launcher.confirming = request
                launcherModel: lm
                onDone: launcher.close()
            }
        }

        Component {
            id: emojiScreen

            EmojiScreen {
                launcherModel: lm
                onDone: launcher.close()
            }
        }

        Component {
            id: filesScreen

            FilesScreen {
                launcherModel: lm
                onDone: launcher.close()
            }
        }

        Component {
            id: notesScreen

            NotesScreen {
                launcherModel: lm
                onDone: launcher.close()
            }
        }

        Component {
            id: outputScreen

            OutputScreen {
                launcherModel: lm
            }
        }

        Component {
            id: notYet

            Item {
                readonly property var hints: []
                function handleKey(event: var): bool {
                    return false;
                }
            }
        }

        // --- the footer ---------------------------------------------------------------------

        Rectangle {
            id: footerRule

            anchors.bottom: footer.top
            width: parent.width
            height: 1
            color: Theme.palette.separator
        }

        Item {
            id: footer

            readonly property var primary: launcher.listScreen && lm.count > 0 ? lm.actions(lm.current, "")[0] ?? null : null

            anchors.bottom: parent.bottom
            width: parent.width
            height: 42

            Row {
                anchors.left: parent.left
                anchors.leftMargin: 18
                anchors.verticalCenter: parent.verticalCenter
                spacing: 8

                // The root advertises its one hop: Tab to Clipboard History.
                StyledText {
                    anchors.verticalCenter: parent.verticalCenter
                    text: lm.screen === "root" ? qsTr("Clipboard History") : (launcher.screenTitles[lm.screen] ?? "")
                    font.pointSize: Theme.font.size.smaller
                    color: Theme.palette.secondaryLabel
                }

                Keycap {
                    anchors.verticalCenter: parent.verticalCenter
                    visible: lm.screen === "root" && lm.arguments.length === 0
                    keys: "Tab"
                    dim: true
                }
            }

            Row {
                anchors.right: parent.right
                anchors.rightMargin: 14
                anchors.verticalCenter: parent.verticalCenter
                spacing: 14

                // The screen's own keys, or the row's action and the menu.
                Repeater {
                    model: launcher.listScreen ? (footer.primary ? [[footer.primary.title, "Enter"], ["Actions", "Ctrl+K"]] : [])
                                               : (body.item?.hints ?? [])

                    Item {
                        id: hint

                        required property var modelData

                        implicitWidth: hintRow.implicitWidth
                        implicitHeight: hintRow.implicitHeight

                        Row {
                            id: hintRow

                            spacing: 6

                            StyledText {
                                anchors.verticalCenter: parent.verticalCenter
                                text: hint.modelData[0]
                                font.pointSize: Theme.font.size.smaller
                                color: Theme.palette.secondaryLabel
                            }

                            Keycap {
                                anchors.verticalCenter: parent.verticalCenter
                                keys: hint.modelData[1]
                            }
                        }

                        // A hint with something to run can be clicked too.
                        MouseArea {
                            anchors.fill: parent
                            visible: typeof hint.modelData[2] === "function"
                            cursorShape: Qt.PointingHandCursor
                            onClicked: hint.modelData[2]()
                        }
                    }
                }
            }
        }

        // --- the actions menu (Ctrl+K) --------------------------------------------------------

        Rectangle {
            id: menu

            x: parent.width - width - 10
            y: launcher.menuSource === "filter" ? header.height + 4 : footer.y - height - 6
            width: launcher.menuSource === "filter" ? 220 : 330
            height: Math.min(menuColumn.implicitHeight, panel.height - header.height - footer.height - 20)
            radius: 14
            visible: launcher.menuOpen
            color: Theme.material.thick
            border.width: 1
            border.color: Theme.palette.separator
            clip: true

            layer.enabled: true
            layer.effect: MultiEffect {
                shadowEnabled: true
                shadowColor: Theme.palette.shadow
                shadowBlur: 0.8
                shadowVerticalOffset: 4
            }

            MouseArea {
                anchors.fill: parent
            }

            Column {
                id: menuColumn

                width: parent.width
                topPadding: 6
                bottomPadding: 0

                Repeater {
                    model: launcher.menuActions

                    Column {
                        id: item

                        required property var modelData
                        required property int index

                        width: menuColumn.width

                        Rectangle {
                            visible: item.modelData.group
                            x: 8
                            width: parent.width - 16
                            height: 1
                            color: Theme.palette.separator
                        }

                        Item {
                            width: parent.width
                            height: 36

                            Rectangle {
                                anchors.fill: parent
                                anchors.leftMargin: 6
                                anchors.rightMargin: 6
                                radius: 8
                                color: item.index === launcher.menuCurrent ? Theme.palette.accentFill : "transparent"
                            }

                            MaterialIcon {
                                id: menuGlyph

                                anchors.left: parent.left
                                anchors.leftMargin: 16
                                anchors.verticalCenter: parent.verticalCenter
                                text: item.modelData.glyph
                                color: Theme.palette.secondaryLabel
                            }

                            StyledText {
                                anchors.left: menuGlyph.right
                                anchors.leftMargin: 10
                                anchors.right: menuKeys.left
                                anchors.rightMargin: 8
                                anchors.verticalCenter: parent.verticalCenter
                                text: item.modelData.title
                                elide: Text.ElideRight
                                color: item.modelData.id === "delete" ? Theme.palette.red : Theme.palette.label
                            }

                            Keycap {
                                id: menuKeys

                                anchors.right: parent.right
                                anchors.rightMargin: 14
                                anchors.verticalCenter: parent.verticalCenter
                                keys: item.modelData.keys
                                dim: true
                            }

                            MouseArea {
                                anchors.fill: parent
                                hoverEnabled: true
                                onPositionChanged: launcher.menuCurrent = item.index
                                onClicked: launcher.runMenu(item.index)
                            }
                        }
                    }
                }

                StyledText {
                    visible: launcher.menuActions.length === 0
                    width: parent.width
                    height: 36
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                    text: qsTr("No Results")
                    color: Theme.palette.secondaryLabel
                }

                Rectangle {
                    width: parent.width
                    height: 1
                    color: Theme.palette.separator
                }

                // Menus search too.
                TextInput {
                    id: menuSearch

                    selectionColor: Theme.palette.accent
                    selectedTextColor: Theme.palette.labelOnAccent
                    width: parent.width
                    height: 38
                    leftPadding: 16
                    rightPadding: 16
                    verticalAlignment: TextInput.AlignVCenter
                    color: Theme.palette.label
                    font.family: Theme.font.sans
                    font.pointSize: Theme.font.size.normal
                    clip: true
                    onTextChanged: {
                        launcher.menuActions = launcher.menuItems(text);
                        launcher.menuCurrent = 0;
                    }

                    Keys.onPressed: event => {
                        event.accepted = true;
                        const n = launcher.menuActions.length;
                        if (event.key === Qt.Key_Escape) {
                            if (text.length > 0)
                                text = "";
                            else
                                launcher.closeMenu();
                        } else if ((event.modifiers & Qt.ControlModifier) && event.key === Qt.Key_K) {
                            launcher.closeMenu();
                        } else if (event.key === Qt.Key_Down || ((event.modifiers & Qt.ControlModifier) && event.key === Qt.Key_N)) {
                            launcher.menuCurrent = n ? (launcher.menuCurrent + 1) % n : 0;
                        } else if (event.key === Qt.Key_Up || ((event.modifiers & Qt.ControlModifier) && event.key === Qt.Key_P)) {
                            launcher.menuCurrent = n ? (launcher.menuCurrent - 1 + n) % n : 0;
                        } else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
                            launcher.runMenu(launcher.menuCurrent);
                        } else {
                            event.accepted = false;
                        }
                    }

                    StyledText {
                        anchors.fill: parent
                        leftPadding: 16
                        verticalAlignment: Text.AlignVCenter
                        visible: menuSearch.text.length === 0 && menuSearch.preeditText.length === 0
                        text: launcher.menuSource === "filter" ? qsTr("Search types…") : qsTr("Search for actions…")
                        color: Theme.palette.tertiaryLabel
                    }
                }
            }
        }

        // --- asking first -----------------------------------------------------------------------

        Rectangle {
            anchors.fill: parent
            radius: panel.radius
            visible: launcher.confirming !== null
            color: Theme.palette.scrim

            MouseArea {
                anchors.fill: parent
            }

            Rectangle {
                anchors.centerIn: parent
                width: 320
                height: askColumn.implicitHeight + 36
                radius: 18
                color: Theme.material.thick
                border.width: 1
                border.color: Theme.palette.separator

                Column {
                    id: askColumn

                    anchors.centerIn: parent
                    width: parent.width - 36
                    spacing: 10

                    MaterialIcon {
                        anchors.horizontalCenter: parent.horizontalCenter
                        text: launcher.confirming?.glyph ?? ""
                        font.pointSize: 26
                        color: Theme.palette.accent
                    }

                    StyledText {
                        width: parent.width
                        horizontalAlignment: Text.AlignHCenter
                        text: launcher.confirming?.title ?? ""
                        wrapMode: Text.Wrap
                        font.weight: Font.Medium
                    }

                    StyledText {
                        width: parent.width
                        visible: text.length > 0
                        horizontalAlignment: Text.AlignHCenter
                        text: launcher.confirming?.detail ?? ""
                        wrapMode: Text.Wrap
                        font.pointSize: Theme.font.size.smaller
                        color: Theme.palette.secondaryLabel
                    }

                    Row {
                        anchors.horizontalCenter: parent.horizontalCenter
                        spacing: 10

                        DialogButton {
                            text: qsTr("Cancel")
                            onClicked: launcher.confirming = null
                        }

                        DialogButton {
                            text: qsTr("Confirm")
                            primary: true
                            onClicked: launcher.confirmNow()
                        }
                    }
                }
            }
        }
    }

    FeedbackPill {
        id: pill
    }
}
