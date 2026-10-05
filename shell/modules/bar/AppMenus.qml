pragma ComponentBehavior: Bound

import QtQuick
import shell.components
import shell.services
import Atrium

// The focused app's own menus (File, Edit, View…) after its name, as on a
// Mac. A click opens one; while one is open, moving along the titles (or
// Left and Right) opens the next.
Row {
    id: root

    required property var screen
    readonly property bool shown: AppMenu.menus.length > 0
    // The room before the bar's right side: the titles past it are left out.
    property real maxWidth: 10000
    // How many titles fit; when not all, room is kept for » with the rest.
    readonly property int fitting: {
        let width = 0;
        for (let i = 0; i < titles.count; ++i)
            width += titles.itemAt(i)?.implicitWidth ?? 0;
        if (width <= maxWidth)
            return titles.count;
        width = 0;
        for (let i = 0; i < titles.count; ++i) {
            width += titles.itemAt(i)?.implicitWidth ?? 0;
            if (width > maxWidth - more.implicitWidth)
                return i;
        }
        return titles.count;
    }
    // The menus left out, as » lists them.
    readonly property var rest: AppMenu.menus.slice(fitting).map(m => ({ id: m.id, text: m.text, enabled: m.enabled, submenu: true, children: [], separator: false, checked: false, shortcut: "" }))

    // Each title's place in the bar, for the panel to follow the pointer along them.
    function spans(): var {
        const out = [];
        for (let i = 0; i < root.fitting; ++i) {
            const t = titles.itemAt(i);
            const p = t.mapToItem(null, 0, 0);
            out.push({ x: p.x, width: t.width, bottom: p.y + t.height, menu: t.modelData });
        }
        if (more.visible) {
            const p = more.mapToItem(null, 0, 0);
            out.push({ x: p.x, width: more.width, bottom: p.y + more.height, menu: null, entries: root.rest });
        }
        return out;
    }

    function open(index: int): void {
        const all = spans();
        if (index < 0 || index >= all.length)
            return;
        panel.show(all, index);
    }

    spacing: 0
    visible: shown

    Repeater {
        id: titles

        model: AppMenu.menus

        Rectangle {
            id: title

            required property var modelData
            required property int index
            readonly property bool current: panel.visible && panel.index === index

            visible: index < root.fitting
            implicitWidth: label.implicitWidth + Theme.padding.normal * 2
            implicitHeight: Theme.bar.inner
            radius: Theme.rounding.small
            color: current ? Theme.palette.secondaryFill : area.containsMouse ? Theme.palette.tertiaryFill : "transparent"

            StyledText {
                id: label

                anchors.centerIn: parent
                text: title.modelData.text
                opacity: title.modelData.enabled ? 1 : 0.4
            }

            MouseArea {
                id: area

                anchors.fill: parent
                hoverEnabled: true
                onClicked: title.current ? panel.close() : root.open(title.index)
            }
        }
    }

    Rectangle {
        id: more

        readonly property bool current: panel.visible && panel.index === root.fitting

        visible: root.fitting < titles.count
        implicitWidth: moreLabel.implicitWidth + Theme.padding.normal * 2
        implicitHeight: Theme.bar.inner
        radius: Theme.rounding.small
        color: current ? Theme.palette.secondaryFill : moreArea.containsMouse ? Theme.palette.tertiaryFill : "transparent"

        StyledText {
            id: moreLabel

            anchors.centerIn: parent
            text: "»"
        }

        MouseArea {
            id: moreArea

            anchors.fill: parent
            hoverEnabled: true
            onClicked: more.current ? panel.close() : root.open(root.fitting)
        }
    }

    AppMenuPanel {
        id: panel

        screen: root.screen
    }
}
