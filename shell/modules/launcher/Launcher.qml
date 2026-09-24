pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Effects
import Atrium.Shell
import shell.components
import shell.services
import Atrium

// Spotlight: type to find apps (and open windows), do sums, or ">" to run a
// command. Opened by the "shell launcher" action (Super+Space).
PanelWindow {
    id: launcher

    LauncherResults {
        id: results

        entries: DesktopEntries
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

    function toggle(): void {
        visible ? close() : open();
    }

    function open(): void {
        input.text = "";
        visible = true;
        input.forceActiveFocus();
        shown.restart();
    }

    function close(): void {
        visible = false;
    }

    function activate(row: int): void {
        if (results.activate(row))
            close();
    }

    Connections {
        target: Atrium

        function onShellAction(name: string): void {
            if (name === "launcher")
                launcher.toggle();
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
        y: Math.round(launcher.height * 0.2)
        width: Math.min(680, launcher.width - 64)
        height: column.implicitHeight
        radius: 26
        color: Theme.panel(Theme.palette.m3SurfaceContainer, 0.78)

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
                    onTextChanged: results.query = text

                    Keys.onEscapePressed: launcher.close()
                    Keys.onDownPressed: results.move(1)
                    Keys.onUpPressed: results.move(-1)
                    Keys.onTabPressed: results.move(1)
                    Keys.onBacktabPressed: results.move(-1)
                    Keys.onReturnPressed: launcher.activate(-1)
                    Keys.onEnterPressed: launcher.activate(-1)

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
                visible: results.count > 0
                width: parent.width
                height: 1
                color: Theme.alpha(Theme.palette.m3Outline, 0.2)
            }

            Column {
                width: parent.width
                topPadding: results.count > 0 ? 6 : 0
                bottomPadding: results.count > 0 ? 6 : 0

                Repeater {
                    model: results

                    Item {
                        id: row

                        required property int index
                        required property string kind
                        required property string title
                        required property string subtitle
                        required property string icon
                        required property string glyph
                        readonly property bool selected: index === results.current

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
                            visible: row.icon.length > 0
                            source: row.icon ? (row.icon.startsWith("file:") ? row.icon : Shell.iconPath(row.icon, "application-x-executable")) : ""
                            asynchronous: true
                        }

                        MaterialIcon {
                            anchors.centerIn: appIcon
                            visible: row.icon.length === 0
                            text: row.glyph
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
                                text: row.title
                                elide: Text.ElideRight
                                font.pointSize: row.kind === "calc" ? 16 : Theme.font.size.normal
                                font.weight: Font.Medium
                            }

                            StyledText {
                                width: parent.width
                                text: row.subtitle
                                elide: Text.ElideRight
                                font.pointSize: Theme.font.size.small
                                color: Theme.palette.m3OnSurfaceVariant
                            }
                        }

                        MouseArea {
                            anchors.fill: parent
                            hoverEnabled: true
                            onEntered: results.current = row.index
                            onClicked: launcher.activate(row.index)
                        }
                    }
                }
            }
        }
    }
}
