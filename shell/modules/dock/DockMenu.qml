pragma ComponentBehavior: Bound

import QtQuick
import Atrium.Shell
import shell.components
import shell.services
import Atrium

// An app's menu, opening above its icon.
Rectangle {
    id: root

    property var item: null  // the DockItem it belongs to
    signal closed

    readonly property var actions: {
        if (!item)
            return [];
        const list = [];
        if (item.isTile)
            return list;
        if (item.running || item.fixed)
            list.push({ icon: "add", text: qsTr("New Window"), action: "launch" });
        if (item.windowCount > 1)
            list.push({ icon: "select_window", text: qsTr("Show All Windows"), action: "expose" });
        if (item.apps.otherGpu && (Atrium.settings["dock.other_gpu"] ?? false))
            list.push({ icon: "memory", text: qsTr("Launch on %1").arg(item.apps.otherGpu), action: "launch-gpu" });
        if (!item.fixed)  // the file manager always stays
            list.push(item.pinned ? { icon: "keep_off", text: qsTr("Remove from Dock"), action: "unpin" }
                                  : { icon: "keep", text: qsTr("Keep in Dock"), action: "pin" });
        if (item.running)
            list.push({ icon: "close", text: item.windowCount > 1 ? qsTr("Close %1 Windows").arg(item.windowCount) : qsTr("Quit"),
                        action: "close" });
        return list;
    }

    // The app's own rows first: its places or recent files, its desktop
    // file's actions, its player and sound (DockApps.menuExtras).
    readonly property var extras: !item ? [] : item.isTile ? item.menuRows : item.apps.menuExtras(item.appId)
    readonly property var rows: extras.concat(actions)

    function perform(action: string): void {
        const apps = item.apps;
        if (action === "launch")
            apps.launch(item.appId);
        else if (action === "launch-gpu")
            apps.launchOnOtherGpu(item.appId);
        else if (action === "pin" || action === "unpin")
            apps.setPinned(item.appId, action === "pin");
        else if (action === "expose")
            apps.expose(item.appId);
        else if (action === "close")
            apps.closeAll(item.appId);
    }

    visible: item !== null
    // As wide as its widest row ("Launch on GeForce RTX 5060 Ti"), 200 at least.
    // The kit's menu, as Menu's: rounded 16, rows lit in the accent.
    width: Math.max(200, ...column.children.map(c => c.need ?? 0)) + Theme.padding.smaller * 2
    height: column.implicitHeight + Theme.padding.smaller * 2
    radius: 16
    color: Theme.material.regular

    Glass {}
    border.width: Theme.lens ? 0 : 1
    border.color: Theme.palette.separator

    Column {
        id: column

        anchors.fill: parent
        anchors.margins: Theme.padding.smaller

        StyledText {
            width: parent.width
            height: 32
            leftPadding: Theme.padding.normal
            text: root.item?.name ?? ""
            font.weight: Font.Bold
            color: Theme.palette.secondaryLabel
        }

        Repeater {
            model: root.rows

            Item {
                id: row

                required property var modelData
                readonly property bool separator: modelData.separator ?? false
                readonly property bool section: modelData.section !== undefined
                readonly property bool media: modelData.media ?? false
                readonly property real need: separator || media ? 0 : rowContent.implicitWidth + Theme.padding.normal * 2 + Theme.spacing.large + (check.visible ? check.width + Theme.spacing.small : 0)

                width: column.width
                height: separator ? 15 : section ? 28 : media ? 40 : 32

                function pick(key: string): void {
                    const item = root.item;
                    root.closed();
                    if (item.isTile)
                        item.pick(key);
                    else
                        item.apps.menuPick(item.appId, key);
                }

                Rectangle {
                    visible: row.separator
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.margins: Theme.padding.normal + Theme.padding.small
                    height: 1
                    color: Theme.palette.separator
                }

                // A heading over the app's rows ("Recent Files").
                StyledText {
                    visible: row.section
                    anchors.left: parent.left
                    anchors.leftMargin: Theme.padding.normal
                    anchors.bottom: parent.bottom
                    anchors.bottomMargin: 4
                    text: row.modelData.section ?? ""
                    color: Theme.palette.secondaryLabel
                    font.weight: Font.Bold
                }

                // The player's keys, in one row.
                Row {
                    visible: row.media
                    anchors.centerIn: parent
                    spacing: Theme.spacing.large

                    Repeater {
                        model: [
                            { key: "media:previous", glyph: "skip_previous", on: row.modelData.canPrevious ?? false },
                            { key: "media:toggle", glyph: (row.modelData.playing ?? false) ? "pause" : "play_arrow", on: row.modelData.canToggle ?? false },
                            { key: "media:next", glyph: "skip_next", on: row.modelData.canNext ?? false }
                        ]

                        Rectangle {
                            id: mediaKey

                            required property var modelData

                            width: 36
                            height: 32
                            radius: Theme.rounding.small
                            color: keyArea.containsMouse && modelData.on ? Theme.palette.tertiaryFill : "transparent"
                            opacity: modelData.on ? 1 : 0.35

                            MaterialIcon {
                                anchors.centerIn: parent
                                text: mediaKey.modelData.glyph
                                fill: 1
                                font.pointSize: Theme.font.size.larger
                            }

                            MouseArea {
                                id: keyArea

                                anchors.fill: parent
                                hoverEnabled: true
                                enabled: mediaKey.modelData.on
                                onClicked: row.pick(mediaKey.modelData.key)
                            }
                        }
                    }
                }

                Rectangle {
                    visible: !row.separator && !row.section && !row.media
                    enabled: row.modelData.enabled ?? true
                    opacity: enabled ? 1 : 0.4
                    anchors.fill: parent
                    radius: 11
                    readonly property bool lit: hover.containsMouse && enabled
                    color: lit ? Theme.palette.accent : "transparent"

                    Row {
                        id: rowContent

                        anchors.verticalCenter: parent.verticalCenter
                        anchors.left: parent.left
                        anchors.leftMargin: Theme.padding.normal
                        spacing: Theme.spacing.small

                        MaterialIcon {
                            anchors.verticalCenter: parent.verticalCenter
                            visible: !row.modelData.themeIcon
                            text: row.modelData.icon ?? row.modelData.glyph ?? ""
                            font.pointSize: Theme.font.size.normal
                            color: rowContent.parent.lit ? Theme.palette.labelOnAccent : Theme.palette.label
                        }

                        IconImage {
                            anchors.verticalCenter: parent.verticalCenter
                            visible: !!row.modelData.themeIcon
                            implicitSize: 18
                            source: row.modelData.themeIcon ? Shell.iconPath(row.modelData.themeIcon, "application-x-executable") : ""
                            asynchronous: true
                        }

                        StyledText {
                            anchors.verticalCenter: parent.verticalCenter
                            width: Math.min(implicitWidth, 320)
                            elide: Text.ElideMiddle
                            text: row.modelData.text ?? ""
                            font.weight: Font.Medium
                            color: rowContent.parent.lit ? Theme.palette.labelOnAccent : Theme.palette.label
                        }
                    }

                    MaterialIcon {
                        id: check

                        anchors.verticalCenter: parent.verticalCenter
                        anchors.right: parent.right
                        anchors.rightMargin: Theme.padding.normal
                        visible: row.modelData.checkable ?? false
                        text: "check"
                        opacity: (row.modelData.checked ?? false) ? 1 : 0
                        color: parent.lit ? Theme.palette.labelOnAccent : Theme.palette.accent
                    }

                    MouseArea {
                        id: hover

                        anchors.fill: parent
                        hoverEnabled: true
                        onClicked: {
                            if (row.modelData.key !== undefined) {
                                row.pick(row.modelData.key);
                            } else {
                                root.perform(row.modelData.action);
                                root.closed();
                            }
                        }
                    }
                }
            }
        }
    }
}
