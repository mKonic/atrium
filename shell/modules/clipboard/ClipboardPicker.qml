pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Effects
import Quickshell
import Quickshell.Wayland
import qs.components
import qs.services
import Atrium

// Super+V: what you copied, searchable, with the whole thing previewed on
// the right. Enter copies it back; Delete forgets it.
PanelWindow {
    id: picker

    property int current: 0
    readonly property var entries: Clipboard.results
    readonly property var selected: entries[current] ?? null

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
    WlrLayershell.namespace: "atrium-clipboard"

    function open(): void {
        Clipboard.query = "";
        search.text = "";
        current = 0;
        Clipboard.refresh();
        visible = true;
        search.forceActiveFocus();
        shown.restart();
    }

    function close(): void {
        visible = false;
    }

    function pick(): void {
        if (!selected)
            return;
        Clipboard.copy(selected.id);
        close();
    }

    onSelectedChanged: {
        if (selected)
            Clipboard.showPreview(selected.id);
    }
    onEntriesChanged: current = Math.min(current, Math.max(0, entries.length - 1))

    Connections {
        target: Atrium

        function onShellAction(name: string): void {
            if (name === "clipboard")
                picker.visible ? picker.close() : picker.open();
        }
    }

    MouseArea {
        anchors.fill: parent
        onClicked: picker.close()
    }

    Rectangle {
        id: panel

        anchors.horizontalCenter: parent.horizontalCenter
        y: Math.round(picker.height * 0.16)
        width: Math.min(860, picker.width - 64)
        height: Math.min(540, picker.height - y - 48)
        radius: 26
        color: Theme.panel(Theme.palette.m3SurfaceContainer, 0.8)

        GlassRim {}

        border.width: 1
        border.color: Theme.alpha(Theme.palette.m3Outline, 0.22)

        layer.enabled: true
        layer.effect: MultiEffect {
            shadowEnabled: true
            shadowColor: Qt.rgba(0, 0, 0, 0.5)
            shadowBlur: 1
            shadowVerticalOffset: 8
        }

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

        // Search, and the count / Clear All on the right.
        Item {
            id: header

            width: parent.width
            height: 60

            MaterialIcon {
                id: glyph

                anchors.left: parent.left
                anchors.leftMargin: 20
                anchors.verticalCenter: parent.verticalCenter
                text: "content_paste_search"
                font.pointSize: 18
                color: Theme.palette.m3OnSurfaceVariant
            }

            TextInput {
                id: search

                anchors.left: glyph.right
                anchors.leftMargin: 12
                anchors.right: tools.left
                anchors.rightMargin: 12
                anchors.verticalCenter: parent.verticalCenter
                color: Theme.palette.m3OnSurface
                font.family: Theme.font.sans
                font.pointSize: 16
                selectionColor: Theme.palette.m3Primary
                selectedTextColor: Theme.palette.m3OnPrimary
                clip: true
                onTextChanged: {
                    Clipboard.query = text;
                    picker.current = 0;
                }

                Keys.onPressed: event => {
                    const n = picker.entries.length;
                    if (event.key === Qt.Key_Escape)
                        picker.close();
                    else if (event.key === Qt.Key_Down)
                        picker.current = Math.min(n - 1, picker.current + 1);
                    else if (event.key === Qt.Key_Up)
                        picker.current = Math.max(0, picker.current - 1);
                    else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter)
                        picker.pick();
                    else if (event.key === Qt.Key_Delete && picker.selected)
                        Clipboard.remove(picker.selected.id);
                    else
                        return;
                    event.accepted = true;
                    list.positionViewAtIndex(picker.current, ListView.Contain);
                }

                StyledText {
                    anchors.fill: parent
                    visible: search.text.length === 0
                    text: "Search clipboard"
                    font.pointSize: 16
                    color: Theme.alpha(Theme.palette.m3OnSurfaceVariant, 0.6)
                }
            }

            Row {
                id: tools

                anchors.right: parent.right
                anchors.rightMargin: 16
                anchors.verticalCenter: parent.verticalCenter
                spacing: 8

                StyledText {
                    anchors.verticalCenter: parent.verticalCenter
                    text: `${picker.entries.length} items`
                    font.pointSize: Theme.font.size.smaller
                    color: Theme.palette.m3OnSurfaceVariant
                }

                Rectangle {
                    visible: picker.entries.length > 0
                    width: clearLabel.implicitWidth + 20
                    height: 28
                    radius: 14
                    color: clearArea.containsMouse ? Theme.palette.m3SurfaceContainerHigh : "transparent"

                    StyledText {
                        id: clearLabel

                        anchors.centerIn: parent
                        text: "Clear All"
                        font.pointSize: Theme.font.size.smaller
                        color: Theme.palette.m3Primary
                    }

                    MouseArea {
                        id: clearArea

                        anchors.fill: parent
                        hoverEnabled: true
                        onClicked: Clipboard.clear()
                    }
                }
            }
        }

        Rectangle {
            anchors.top: header.bottom
            width: parent.width
            height: 1
            color: Theme.alpha(Theme.palette.m3Outline, 0.2)
        }

        // Left: the history.
        ListView {
            id: list

            anchors.top: header.bottom
            anchors.topMargin: 8
            anchors.bottom: parent.bottom
            anchors.bottomMargin: 8
            anchors.left: parent.left
            width: 330
            clip: true
            model: picker.entries
            currentIndex: picker.current
            boundsBehavior: Flickable.StopAtBounds
            reuseItems: true

            delegate: Item {
                id: row

                required property var modelData
                required property int index
                readonly property bool selected: index === picker.current

                width: list.width
                height: modelData.image ? 64 : 44

                Rectangle {
                    anchors.fill: parent
                    anchors.leftMargin: 8
                    anchors.rightMargin: 4
                    radius: 12
                    color: row.selected ? Theme.alpha(Theme.palette.m3Primary, 0.22)
                         : rowArea.containsMouse ? Theme.alpha(Theme.palette.m3OnSurface, 0.06) : "transparent"
                }

                MaterialIcon {
                    id: kind

                    anchors.left: parent.left
                    anchors.leftMargin: 20
                    anchors.verticalCenter: parent.verticalCenter
                    visible: !row.modelData.image || !row.modelData.thumb
                    text: row.modelData.image ? "image" : "notes"
                    font.pointSize: Theme.font.size.normal
                    color: Theme.palette.m3OnSurfaceVariant
                }

                Image {
                    anchors.left: parent.left
                    anchors.leftMargin: 18
                    anchors.verticalCenter: parent.verticalCenter
                    width: 72
                    height: 48
                    visible: row.modelData.image && !!row.modelData.thumb
                    source: row.modelData.thumb
                    sourceSize.width: 144
                    sourceSize.height: 96
                    fillMode: Image.PreserveAspectFit
                    asynchronous: true
                }

                StyledText {
                    anchors.left: parent.left
                    anchors.leftMargin: row.modelData.image ? 100 : 48
                    anchors.right: parent.right
                    anchors.rightMargin: 16
                    anchors.verticalCenter: parent.verticalCenter
                    text: row.modelData.image ? `${row.modelData.width}×${row.modelData.height} ${row.modelData.format.toUpperCase()} · ${row.modelData.size}`
                                              : row.modelData.text.trim()
                    elide: Text.ElideRight
                    maximumLineCount: 1
                    font.pointSize: Theme.font.size.smaller
                    color: row.modelData.image ? Theme.palette.m3OnSurfaceVariant : Theme.palette.m3OnSurface
                }

                // Hover previews it, a click copies it; arrows and Enter do the same.
                MouseArea {
                    id: rowArea

                    anchors.fill: parent
                    hoverEnabled: true
                    onEntered: picker.current = row.index
                    onClicked: {
                        picker.current = row.index;
                        picker.pick();
                    }
                }
            }
        }

        Rectangle {
            anchors.top: header.bottom
            anchors.bottom: parent.bottom
            anchors.left: list.right
            width: 1
            color: Theme.alpha(Theme.palette.m3Outline, 0.2)
        }

        // Right: all of it.
        Item {
            id: previewPane

            readonly property var p: Clipboard.preview

            anchors.top: header.bottom
            anchors.bottom: footer.top
            anchors.left: list.right
            anchors.right: parent.right
            anchors.margins: 20
            clip: true

            Image {
                anchors.fill: parent
                visible: previewPane.p.image === true
                source: previewPane.p.image ? previewPane.p.thumb : ""
                fillMode: Image.PreserveAspectFit
                asynchronous: true
                smooth: true
                mipmap: true
            }

            Flickable {
                anchors.fill: parent
                visible: previewPane.p.image === false
                contentHeight: full.implicitHeight
                boundsBehavior: Flickable.StopAtBounds

                Text {
                    id: full

                    width: parent.width
                    text: previewPane.p.full ?? previewPane.p.text ?? ""
                    wrapMode: Text.Wrap
                    color: Theme.palette.m3OnSurface
                    font.family: Theme.font.mono
                    font.pointSize: Theme.font.size.smaller
                    textFormat: Text.PlainText
                }
            }

            Column {
                anchors.centerIn: parent
                visible: picker.entries.length === 0
                spacing: 8

                MaterialIcon {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: "content_paste_off"
                    font.pointSize: 28
                    color: Theme.alpha(Theme.palette.m3OnSurfaceVariant, 0.6)
                }

                StyledText {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: Clipboard.available ? "Nothing copied yet" : "Clipboard history needs cliphist"
                    color: Theme.palette.m3OnSurfaceVariant
                }
            }
        }

        // What Enter and Delete do.
        Row {
            id: footer

            anchors.bottom: parent.bottom
            anchors.bottomMargin: 14
            anchors.right: parent.right
            anchors.rightMargin: 20
            spacing: 16
            visible: picker.selected !== null

            Repeater {
                model: [["Copy", "↵"], ["Delete", "Del"]]

                Row {
                    required property var modelData

                    spacing: 6

                    StyledText {
                        text: parent.modelData[0]
                        font.pointSize: Theme.font.size.small
                        color: Theme.palette.m3OnSurfaceVariant
                    }

                    Rectangle {
                        width: key.implicitWidth + 10
                        height: key.implicitHeight + 2
                        radius: 5
                        color: Theme.palette.m3SurfaceContainerHigh

                        StyledText {
                            id: key

                            anchors.centerIn: parent
                            text: parent.parent.modelData[1]
                            font.pointSize: Theme.font.size.small
                        }
                    }
                }
            }
        }
    }
}
