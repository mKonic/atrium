pragma ComponentBehavior: Bound

import QtQuick
import Atrium.Shell
import Atrium
import shell.components
import shell.services

Pill {
    id: root

    required property var bar  // the panel window, for placing menus

    visible: items.count > 0
    implicitWidth: row.implicitWidth + Theme.padding.normal * 2

    TrayMenu {
        id: menu

        screen: root.bar.screen
    }

    Row {
        id: row

        anchors.centerIn: parent
        spacing: Theme.spacing.small

        Repeater {
            id: items

            model: SystemTray.items

            MouseArea {
                id: item

                required property SystemTrayItem modelData

                implicitWidth: 18
                implicitHeight: 18
                acceptedButtons: Qt.LeftButton | Qt.RightButton

                onClicked: event => {
                    if (event.button === Qt.LeftButton && !modelData.onlyMenu)
                        modelData.activate();
                    else if (modelData.hasMenu) {
                        const p = mapToItem(null, 0, height + 6);
                        menu.open(modelData, p.x, p.y);
                    }
                }

                IconImage {
                    anchors.fill: parent
                    source: item.modelData.icon
                    asynchronous: true
                }
            }
        }
    }
}
