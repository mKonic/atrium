pragma ComponentBehavior: Bound

import QtQuick
import shell.components
import shell.services
import Atrium

// Displays as macOS arranges them: the screens drawn to scale, dragged to
// where they sit on the desk; under them, the selected one's resolution,
// refresh rate, scale and rotation. Each monitor is remembered by make,
// model and serial and comes back this way when plugged in.
Rectangle {
    id: root

    property string selected: Atrium.focusedOutput?.name ?? ""
    readonly property var output: Atrium.outputs.find(o => o.name === selected) ?? Atrium.outputs[0] ?? null

    // Every display's box in layout pixels, and the whole arrangement.
    readonly property var bounds: {
        let x0 = 1e9, y0 = 1e9, x1 = -1e9, y1 = -1e9;
        for (const o of Atrium.outputs) {
            const g = o.geometry;
            x0 = Math.min(x0, g.x);
            y0 = Math.min(y0, g.y);
            x1 = Math.max(x1, g.x + g.width);
            y1 = Math.max(y1, g.y + g.height);
        }
        return { x: x0, y: y0, width: Math.max(1, x1 - x0), height: Math.max(1, y1 - y0) };
    }

    // Resolutions it has, largest first, each with its refresh rates.
    readonly property var resolutions: {
        const seen = {};
        for (const m of output?.modes ?? []) {
            const key = `${m.width}x${m.height}`;
            (seen[key] = seen[key] ?? { width: m.width, height: m.height, preferred: false, rates: [] }).rates.push(m.refresh);
            seen[key].preferred = seen[key].preferred || m.preferred;
        }
        return Object.values(seen).sort((a, b) => b.width * b.height - a.width * a.height);
    }

    function configure(fields: var): void {
        if (output)
            Atrium.configureOutput(output.name, fields);
    }

    width: parent?.width ?? 0
    height: column.implicitHeight + 32
    radius: 14
    color: Theme.palette.m3SurfaceContainer
    border.width: 1
    border.color: Theme.alpha(Theme.palette.m3Outline, 0.12)

    Column {
        id: column

        x: 16
        y: 16
        width: parent.width - 32
        spacing: 14

        // --- the arrangement ---
        Rectangle {
            id: desk

            readonly property real scale: Math.min((width - 40) / root.bounds.width, (height - 40) / root.bounds.height)

            width: parent.width
            height: 220
            radius: 10
            color: Theme.alpha(Theme.palette.m3OnSurface, 0.04)

            Repeater {
                model: Atrium.outputs

                Rectangle {
                    id: screen

                    required property var modelData
                    readonly property bool current: root.output?.name === modelData.name
                    readonly property real homeX: 20 + (modelData.geometry.x - root.bounds.x) * desk.scale +
                                                  (desk.width - 40 - root.bounds.width * desk.scale) / 2
                    readonly property real homeY: 20 + (modelData.geometry.y - root.bounds.y) * desk.scale +
                                                  (desk.height - 40 - root.bounds.height * desk.scale) / 2

                    x: homeX
                    y: homeY
                    width: modelData.geometry.width * desk.scale
                    height: modelData.geometry.height * desk.scale
                    radius: 6
                    color: modelData.enabled ? Theme.alpha(Theme.palette.m3Primary, current ? 0.35 : 0.18) : Theme.alpha(Theme.palette.m3OnSurface, 0.08)
                    border.width: current ? 2 : 1
                    border.color: current ? Theme.palette.m3Primary : Theme.alpha(Theme.palette.m3Outline, 0.5)

                    Column {
                        anchors.centerIn: parent
                        width: parent.width - 12

                        StyledText {
                            width: parent.width
                            horizontalAlignment: Text.AlignHCenter
                            text: screen.modelData.model || screen.modelData.name
                            elide: Text.ElideRight
                            font.weight: Font.DemiBold
                            font.pointSize: Theme.font.size.small
                        }

                        StyledText {
                            width: parent.width
                            horizontalAlignment: Text.AlignHCenter
                            text: `${screen.modelData.mode.width} × ${screen.modelData.mode.height}`
                            font.pointSize: Theme.font.size.smaller
                            color: Theme.palette.m3OnSurfaceVariant
                        }
                    }

                    // Drag to rearrange; let go and it snaps against its
                    // neighbours' edges.
                    MouseArea {
                        anchors.fill: parent
                        drag.target: Atrium.outputs.length > 1 ? screen : null
                        cursorShape: Atrium.outputs.length > 1 ? Qt.OpenHandCursor : Qt.ArrowCursor
                        onPressed: root.selected = screen.modelData.name
                        onReleased: {
                            if (!drag.active && screen.x === screen.homeX && screen.y === screen.homeY)
                                return;
                            let lx = Math.round((screen.x - screen.homeX) / desk.scale) + screen.modelData.geometry.x;
                            let ly = Math.round((screen.y - screen.homeY) / desk.scale) + screen.modelData.geometry.y;
                            const w = screen.modelData.geometry.width, h = screen.modelData.geometry.height;
                            const snap = 60 / desk.scale;
                            for (const o of Atrium.outputs) {
                                if (o.name === screen.modelData.name)
                                    continue;
                                const g = o.geometry;
                                for (const [edge, target] of [[lx, g.x + g.width], [lx + w, g.x], [lx, g.x], [lx + w, g.x + g.width]])
                                    if (Math.abs(edge - target) < snap) {
                                        lx += target - edge;
                                        break;
                                    }
                                for (const [edge, target] of [[ly, g.y + g.height], [ly + h, g.y], [ly, g.y], [ly + h, g.y + g.height]])
                                    if (Math.abs(edge - target) < snap) {
                                        ly += target - edge;
                                        break;
                                    }
                            }
                            screen.x = Qt.binding(() => screen.homeX);
                            screen.y = Qt.binding(() => screen.homeY);
                            Atrium.configureOutput(screen.modelData.name, { x: lx, y: ly });
                        }
                    }
                }
            }
        }

        // --- the selected display ---
        SectionHeader {
            width: parent.width
            title: root.output ? (root.output.make ? `${root.output.make} ${root.output.model}` : root.output.name) : ""
            subtitle: root.output ? `${root.output.name} · ${root.output.geometry.width} × ${root.output.geometry.height} points` : ""

            Switch {
                visible: Atrium.outputs.length > 1
                checked: root.output?.enabled ?? false
                onToggled: root.configure({ enabled: !checked })
            }
        }

        Setting {
            label: "Resolution"

            StyledText {
                visible: root.resolutions.length === 0
                anchors.verticalCenter: parent.verticalCenter
                text: root.output ? `${root.output.mode.width} × ${root.output.mode.height}` : ""
                font.pointSize: Theme.font.size.small
            }

            Dropdown {
                visible: root.resolutions.length > 0
                fieldWidth: 220
                value: root.output ? `${root.output.mode.width}x${root.output.mode.height}` : ""
                options: root.resolutions.map(r => ({ value: `${r.width}x${r.height}`, label: `${r.width} × ${r.height}` + (r.preferred ? "  (native)" : "") }))
                onPicked: v => {
                    const r = root.resolutions.find(x => `${x.width}x${x.height}` === v);
                    root.configure({ width: r.width, height: r.height, refresh: Math.max(...r.rates) });
                }
            }
        }

        Setting {
            label: "Refresh rate"

            StyledText {
                visible: root.resolutions.length === 0
                anchors.verticalCenter: parent.verticalCenter
                text: (root.output?.mode.refresh ?? 0) > 0 ? `${(root.output.mode.refresh / 1000).toFixed(root.output.mode.refresh % 1000 ? 2 : 0)} Hz` : "Follows the host"
                font.pointSize: Theme.font.size.small
            }

            Dropdown {
                visible: root.resolutions.length > 0
                readonly property var rates: root.resolutions.find(r => r.width === root.output?.mode.width && r.height === root.output?.mode.height)?.rates ?? []

                fieldWidth: 220
                value: String(root.output?.mode.refresh ?? 0)
                options: [...new Set(rates)].sort((a, b) => b - a).map(r => ({ value: String(r), label: `${(r / 1000).toFixed(r % 1000 ? 2 : 0)} Hz` }))
                onPicked: v => root.configure({ width: root.output.mode.width, height: root.output.mode.height, refresh: Number(v) })
            }
        }

        StyledText {
            visible: root.resolutions.length === 0
            width: parent.width
            wrapMode: Text.WordWrap
            text: "This display doesn't list any modes (it's a window or a virtual display), so its resolution and refresh rate can't be changed here."
            font.pointSize: Theme.font.size.smaller
            color: Theme.palette.m3OnSurfaceVariant
        }

        Setting {
            label: "Scale"

            ChoiceControl {
                value: String(root.output?.scale ?? 1)
                choices: ["1", "1.25", "1.5", "1.75", "2"]
                onPicked: v => root.configure({ scale: Number(v) })
            }
        }

        Setting {
            label: "Rotation"

            Dropdown {
                fieldWidth: 220
                value: String(root.output?.transform ?? 0)
                options: [
                    { value: "0", label: "Standard" },
                    { value: "1", label: "90°" },
                    { value: "2", label: "180°" },
                    { value: "3", label: "270°" }
                ]
                onPicked: v => root.configure({ transform: Number(v) })
            }
        }

        // Only screens that can (FreeSync, G-Sync Compatible, Adaptive-Sync).
        Setting {
            visible: root.output?.adaptive_sync_supported ?? false
            label: "Variable refresh"

            Dropdown {
                fieldWidth: 220
                value: root.output?.adaptive_sync ?? "games"
                options: [
                    { value: "off", label: "Off" },
                    { value: "games", label: "Games only" },
                    { value: "on", label: "Always" }
                ]
                onPicked: v => root.configure({ adaptive_sync: v })
            }

            StyledText {
                anchors.verticalCenter: parent.verticalCenter
                visible: root.output?.adaptive_sync_active ?? false
                text: "On now"
                font.pointSize: Theme.font.size.small
                color: Theme.palette.m3Primary
            }
        }

        StyledText {
            visible: root.output?.adaptive_sync_supported ?? false
            width: parent.width
            wrapMode: Text.WordWrap
            text: "The screen waits for each frame instead of refreshing on a fixed beat: smoother games without tearing. Games only turns it on while a fullscreen game is in front, since some screens flicker with it on the desktop."
            font.pointSize: Theme.font.size.smaller
            color: Theme.palette.m3OnSurfaceVariant
        }
    }

    component Setting: Row {
        property string label
        default property alias controls: holder.data

        spacing: 12

        StyledText {
            anchors.verticalCenter: parent.verticalCenter
            width: 150
            text: parent.label
            font.pointSize: Theme.font.size.small
            color: Theme.palette.m3OnSurfaceVariant
        }

        Row {
            id: holder

            anchors.verticalCenter: parent.verticalCenter
            spacing: 8
        }
    }
}
