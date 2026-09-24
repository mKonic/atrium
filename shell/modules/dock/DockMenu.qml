pragma ComponentBehavior: Bound

import QtQuick
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
        if (item.running)
            list.push({ icon: "add", text: "New Window", action: "launch" });
        if (item.windowCount > 1)
            list.push({ icon: "select_window", text: "Show All Windows", action: "expose" });
        list.push(item.pinned ? { icon: "keep_off", text: "Remove from Dock", action: "unpin" }
                              : { icon: "keep", text: "Keep in Dock", action: "pin" });
        if (item.running)
            list.push({ icon: "close", text: item.windowCount > 1 ? `Close ${item.windowCount} Windows` : "Quit",
                        action: "close" });
        return list;
    }

    function perform(action: string): void {
        const apps = item.apps;
        if (action === "launch")
            apps.launch(item.appId);
        else if (action === "pin" || action === "unpin")
            apps.setPinned(item.appId, action === "pin");
        else if (action === "expose")
            apps.expose(item.appId);
        else if (action === "close")
            apps.closeAll(item.appId);
    }

    visible: item !== null
    width: 200
    height: column.implicitHeight + Theme.padding.small * 2
    radius: Theme.rounding.normal
    color: Theme.material.regular
    border.width: Theme.lens ? 0 : 1
    border.color: Theme.palette.separator

    Column {
        id: column

        anchors.fill: parent
        anchors.margins: Theme.padding.small

        StyledText {
            width: parent.width
            leftPadding: Theme.padding.normal
            topPadding: Theme.padding.small
            bottomPadding: Theme.padding.small
            text: root.item?.name ?? ""
            font.weight: Font.DemiBold
            color: Theme.palette.secondaryLabel
            font.pointSize: Theme.font.size.smaller
        }

        Repeater {
            model: root.actions

            Rectangle {
                id: row

                required property var modelData

                width: column.width
                height: 34
                radius: Theme.rounding.small
                color: hover.containsMouse ? Theme.palette.tertiaryFill : "transparent"

                Row {
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.left: parent.left
                    anchors.leftMargin: Theme.padding.normal
                    spacing: Theme.spacing.small

                    MaterialIcon {
                        anchors.verticalCenter: parent.verticalCenter
                        text: row.modelData.icon
                        font.pointSize: Theme.font.size.normal
                    }

                    StyledText {
                        anchors.verticalCenter: parent.verticalCenter
                        text: row.modelData.text
                    }
                }

                MouseArea {
                    id: hover

                    anchors.fill: parent
                    hoverEnabled: true
                    onClicked: {
                        root.perform(row.modelData.action);
                        root.closed();
                    }
                }
            }
        }
    }
}
