pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Effects
import Quickshell
import Quickshell.Io
import Quickshell.Wayland
import Quickshell.Widgets
import qs.components
import qs.services
import Atrium

// Spotlight: type to find apps (and open windows), do sums, or ">" to run a
// command. Opened by the "shell launcher" action (Super+Space).
PanelWindow {
    id: launcher

    property string query: ""
    property int current: 0
    property var counts: ({})  // app id → launches, for ranking

    readonly property string stateDir: `${Quickshell.env("XDG_STATE_HOME") || Quickshell.env("HOME") + "/.local/state"}/atrium`
    readonly property int maxResults: 8

    visible: false
    screen: Quickshell.screens.find(s => s.name === Atrium.focusedOutput?.name) ?? Quickshell.screens[0]
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

    function toggle(): void {
        visible ? close() : open();
    }

    function open(): void {
        query = "";
        input.text = "";
        current = 0;
        visible = true;
        input.forceActiveFocus();
        shown.restart();
    }

    function close(): void {
        visible = false;
    }

    function launch(item: var): void {
        if (!item)
            return;
        close();
        item.run();
    }

    Connections {
        target: Atrium

        function onShellAction(name: string): void {
            if (name === "launcher")
                launcher.toggle();
        }
    }

    // Launch counts, so what you use most comes first.
    FileView {
        id: store

        path: `${launcher.stateDir}/launcher.json`
        onLoaded: {
            try {
                launcher.counts = JSON.parse(text());
            } catch (e) {}
        }
    }

    function remember(id: string): void {
        const c = Object.assign({}, counts);
        c[id] = (c[id] ?? 0) + 1;
        counts = c;
        Quickshell.execDetached(["mkdir", "-p", stateDir]);
        store.setText(JSON.stringify(c));
    }

    readonly property var results: {
        const q = query.trim();
        const out = [];

        if (q.startsWith(">")) {
            const cmd = q.slice(1).trim();
            if (cmd)
                out.push({ kind: "run", title: cmd, subtitle: "Run command", glyph: "terminal",
                           run: () => Quickshell.execDetached(["sh", "-c", cmd]) });
            return out;
        }

        const sum = Search.calculate(q);
        if (sum)
            out.push({ kind: "calc", title: sum, subtitle: `${q} · Enter copies`, glyph: "calculate",
                       run: () => Quickshell.clipboardText = sum });

        // Apps, best match first; with nothing typed, the ones used most.
        for (const e of Search.rankApps(q, DesktopEntries.applications.values, counts, q ? maxResults : 6))
            out.push({ kind: "app", title: e.name, subtitle: e.genericName || e.comment || "Application",
                       icon: Quickshell.iconPath(e.icon, "application-x-executable"),
                       run: () => {
                           launcher.remember(e.id);
                           e.execute();
                       } });

        // Open windows, to jump to.
        if (q) {
            const wins = Atrium.windows.map(w => ({ w: w, s: Search.bestScore(q, [w.title, Icons.appName(w.app_id)], [100, 100]) }))
                .filter(x => x.s >= 400)
                .sort((a, b) => b.s - a.s)
                .slice(0, 3);
            for (const { w } of wins)
                out.push({ kind: "window", title: w.title || Icons.appName(w.app_id), subtitle: `Switch to ${Icons.appName(w.app_id)} · Space ${w.space}`,
                           icon: Icons.appIcon(w.app_id), run: () => Atrium.focusWindow(w.id) });
        }
        return out.slice(0, maxResults);
    }

    onResultsChanged: current = 0

    // A click beside the panel puts it away.
    MouseArea {
        anchors.fill: parent
        onClicked: launcher.close()
    }

    Rectangle {
        id: panel

        anchors.horizontalCenter: parent.horizontalCenter
        y: Math.round(launcher.height * 0.2)
        width: Math.min(680, launcher.width - 64)
        height: column.implicitHeight
        radius: 26
        color: Theme.panel(Theme.palette.m3SurfaceContainer, 0.78)
        border.width: 1
        border.color: Theme.alpha(Theme.palette.m3Outline, 0.22)

        layer.enabled: true
        layer.effect: MultiEffect {
            shadowEnabled: true
            shadowColor: Qt.rgba(0, 0, 0, 0.5)
            shadowBlur: 1
            shadowVerticalOffset: 8
        }

        // Opens with a small settle.
        scale: 1
        opacity: 1
        ParallelAnimation {
            id: shown

            Anim {
                target: panel
                property: "scale"
                from: 0.96
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
        }

        Column {
            id: column

            width: parent.width

            Item {
                width: parent.width
                height: 60

                MaterialIcon {
                    id: glass

                    anchors.left: parent.left
                    anchors.leftMargin: 20
                    anchors.verticalCenter: parent.verticalCenter
                    text: "search"
                    font.pointSize: 18
                    color: Theme.palette.m3OnSurfaceVariant
                }

                TextInput {
                    id: input

                    anchors.left: glass.right
                    anchors.leftMargin: 12
                    anchors.right: parent.right
                    anchors.rightMargin: 20
                    anchors.verticalCenter: parent.verticalCenter
                    color: Theme.palette.m3OnSurface
                    font.family: Theme.font.sans
                    font.pointSize: 18
                    selectionColor: Theme.palette.m3Primary
                    selectedTextColor: Theme.palette.m3OnPrimary
                    clip: true
                    onTextChanged: launcher.query = text

                    Keys.onPressed: event => {
                        const n = launcher.results.length;
                        if (event.key === Qt.Key_Escape) {
                            launcher.close();
                        } else if (event.key === Qt.Key_Down || (event.key === Qt.Key_Tab && n > 0)) {
                            launcher.current = (launcher.current + 1) % Math.max(n, 1);
                        } else if (event.key === Qt.Key_Up || event.key === Qt.Key_Backtab) {
                            launcher.current = (launcher.current - 1 + n) % Math.max(n, 1);
                        } else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
                            launcher.launch(launcher.results[launcher.current]);
                        } else {
                            return;
                        }
                        event.accepted = true;
                    }

                    StyledText {
                        anchors.fill: parent
                        visible: input.text.length === 0
                        text: "Search"
                        font.pointSize: 18
                        color: Theme.alpha(Theme.palette.m3OnSurfaceVariant, 0.6)
                    }
                }
            }

            Rectangle {
                visible: launcher.results.length > 0
                width: parent.width
                height: 1
                color: Theme.alpha(Theme.palette.m3Outline, 0.2)
            }

            Column {
                width: parent.width
                topPadding: launcher.results.length > 0 ? 6 : 0
                bottomPadding: launcher.results.length > 0 ? 6 : 0

                Repeater {
                    model: launcher.results

                    Item {
                        id: row

                        required property var modelData
                        required property int index
                        readonly property bool selected: index === launcher.current

                        width: column.width
                        height: 50

                        Rectangle {
                            anchors.fill: parent
                            anchors.leftMargin: 6
                            anchors.rightMargin: 6
                            radius: 14
                            color: row.selected ? Theme.alpha(Theme.palette.m3Primary, 0.22) : "transparent"
                        }

                        IconImage {
                            id: appIcon

                            anchors.left: parent.left
                            anchors.leftMargin: 18
                            anchors.verticalCenter: parent.verticalCenter
                            implicitSize: 32
                            visible: !!row.modelData.icon
                            source: row.modelData.icon ?? ""
                            asynchronous: true
                        }

                        MaterialIcon {
                            anchors.centerIn: appIcon
                            visible: !row.modelData.icon
                            text: row.modelData.glyph ?? ""
                            font.pointSize: 18
                            color: Theme.palette.m3Primary
                        }

                        Column {
                            anchors.left: appIcon.right
                            anchors.leftMargin: 14
                            anchors.right: parent.right
                            anchors.rightMargin: 20
                            anchors.verticalCenter: parent.verticalCenter

                            StyledText {
                                width: parent.width
                                text: row.modelData.title
                                elide: Text.ElideRight
                                font.pointSize: row.modelData.kind === "calc" ? 16 : Theme.font.size.normal
                                font.weight: Font.Medium
                            }

                            StyledText {
                                width: parent.width
                                text: row.modelData.subtitle
                                elide: Text.ElideRight
                                font.pointSize: Theme.font.size.small
                                color: Theme.palette.m3OnSurfaceVariant
                            }
                        }

                        MouseArea {
                            anchors.fill: parent
                            hoverEnabled: true
                            onEntered: launcher.current = row.index
                            onClicked: launcher.launch(row.modelData)
                        }
                    }
                }
            }
        }
    }
}
