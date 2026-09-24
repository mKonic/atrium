pragma ComponentBehavior: Bound

import QtQuick
import Atrium
import shell.components
import shell.services

// Drives to mount, unmount and eject: removable ones and internal ones
// outside the system (udisks2). Partitioning stays with a partition editor.
Column {
    id: root

    property string error: ""

    spacing: 20

    Connections {
        target: Disks

        function onFailed(why: string): void {
            root.error = why;
        }
    }

    MissingNote {
        needs: "udisks2"
        explanation: "Drives are mounted and ejected through udisks2."
    }

    StyledText {
        visible: Disks.available && (Disks.disks.length === 0 || root.error !== "")
        width: parent.width
        wrapMode: Text.WordWrap
        text: root.error || "No drives to show. Plug one in and it appears here."
        color: Theme.palette.m3OnSurfaceVariant
    }

    Group {
        visible: Disks.available && Disks.disks.length > 0

        Repeater {
            model: Disks.disks

            DeviceRow {
                id: disk

                required property var modelData

                glyph: modelData.removable ? "usb" : "hard_drive"
                name: modelData.name
                note: modelData.mounted ? `${modelData.size} · ${modelData.mountPoint}` : `${modelData.size} · Not mounted`
                active: modelData.mounted
                onClicked: Disks.open(modelData.path)

                PillButton {
                    visible: disk.modelData.mounted
                    text: "Open"
                    onClicked: Disks.open(disk.modelData.path)
                }

                PillButton {
                    text: disk.modelData.mounted ? "Unmount" : "Mount"
                    onClicked: {
                        root.error = "";
                        disk.modelData.mounted ? Disks.unmount(disk.modelData.path) : Disks.mount(disk.modelData.path);
                    }
                }

                PillButton {
                    visible: disk.modelData.removable
                    icon: "eject"
                    onClicked: {
                        root.error = "";
                        Disks.eject(disk.modelData.path);
                    }
                }
            }
        }
    }
}
