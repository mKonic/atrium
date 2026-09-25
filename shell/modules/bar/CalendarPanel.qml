pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import Atrium.Shell
import shell.components
import shell.services
import Atrium

// The month, from the clock (caelestia's dashboard calendar): today marked,
// the chevrons or the wheel for other months, the title back to this one.
PanelWindow {
    id: root

    property date today: new Date()
    property int month: today.getMonth()
    property int year: today.getFullYear()
    readonly property bool thisMonth: month === today.getMonth() && year === today.getFullYear()

    function step(by: int): void {
        const d = new Date(year, month + by, 1);
        month = d.getMonth();
        year = d.getFullYear();
    }

    visible: Panels.open === "calendar"
    screen: Shell.screen(Atrium.focusedOutput?.name)
    anchors {
        top: true
        right: true
    }
    margins {
        top: 8
        right: 8
    }
    implicitWidth: 300
    implicitHeight: panel.height
    exclusiveZone: 0
    color: "transparent"
    WlrLayershell.layer: WlrLayer.Top
    WlrLayershell.namespace: "atrium-calendar"
    WlrLayershell.keyboardFocus: visible ? WlrKeyboardFocus.OnDemand : WlrKeyboardFocus.None

    // Opens on this month, whatever was looked at last time.
    onVisibleChanged: {
        if (!visible)
            return;
        today = new Date();
        month = today.getMonth();
        year = today.getFullYear();
    }

    // Past midnight while open: the mark moves on.
    SystemClock {
        precision: SystemClock.Hours
        onDateChanged: root.today = new Date()
    }

    Rectangle {
        id: panel

        width: parent.width
        height: body.implicitHeight + 28
        radius: 22
        color: Theme.material.regular
        border.width: Theme.lens ? 0 : 1
        border.color: Theme.palette.separator
        focus: true
        Keys.onEscapePressed: Panels.open = ""
        Keys.onLeftPressed: root.step(-1)
        Keys.onRightPressed: root.step(1)

        Glass {}

        WheelHandler {
            acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
            onWheel: e => root.step(e.angleDelta.y > 0 || e.angleDelta.x > 0 ? -1 : 1)
        }

        Column {
            id: body

            x: 14
            y: 14
            width: parent.width - 28
            spacing: 6

            // Today, in full.
            StyledText {
                leftPadding: 4
                text: Qt.formatDate(root.today, "dddd")
                font.pointSize: Theme.font.size.smaller
                font.weight: Font.DemiBold
                color: Theme.palette.accent
            }

            StyledText {
                leftPadding: 4
                bottomPadding: 6
                text: Qt.formatDate(root.today, "d MMMM yyyy")
                font.pointSize: Theme.font.size.larger
                font.weight: Font.DemiBold
            }

            // Month and year; the chevrons either side of Today.
            Item {
                width: parent.width
                height: 30

                StyledText {
                    anchors.left: parent.left
                    anchors.leftMargin: 4
                    anchors.verticalCenter: parent.verticalCenter
                    text: Qt.locale().standaloneMonthName(root.month) + " " + root.year
                    font.weight: Font.DemiBold
                }

                Row {
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 2

                    component NavButton: Rectangle {
                        id: nav

                        property string icon
                        property string label
                        signal tapped

                        width: label ? navLabel.implicitWidth + 20 : 28
                        height: 28
                        radius: 14
                        color: navHover.hovered ? Theme.palette.secondaryFill : "transparent"

                        Behavior on color {
                            CAnim {}
                        }

                        MaterialIcon {
                            visible: !nav.label
                            anchors.centerIn: parent
                            text: nav.icon
                            font.pointSize: Theme.font.size.normal
                            color: Theme.palette.secondaryLabel
                        }

                        StyledText {
                            id: navLabel

                            visible: !!nav.label
                            anchors.centerIn: parent
                            text: nav.label
                            font.pointSize: Theme.font.size.smaller
                            color: Theme.palette.accent
                        }

                        HoverHandler {
                            id: navHover
                        }

                        TapHandler {
                            onTapped: nav.tapped()
                        }
                    }

                    NavButton {
                        icon: "chevron_left"
                        onTapped: root.step(-1)
                    }

                    NavButton {
                        label: "Today"
                        opacity: root.thisMonth ? 0.4 : 1
                        enabled: !root.thisMonth
                        onTapped: {
                            root.month = root.today.getMonth();
                            root.year = root.today.getFullYear();
                        }
                    }

                    NavButton {
                        icon: "chevron_right"
                        onTapped: root.step(1)
                    }
                }
            }

            DayOfWeekRow {
                width: parent.width
                locale: grid.locale

                delegate: StyledText {
                    required property string shortName

                    horizontalAlignment: Text.AlignHCenter
                    text: shortName
                    font.pointSize: Theme.font.size.small
                    font.weight: Font.Medium
                    color: Theme.palette.tertiaryLabel
                }
            }

            MonthGrid {
                id: grid

                width: parent.width
                month: root.month
                year: root.year
                locale: Qt.locale()
                spacing: 2

                delegate: Item {
                    id: cell

                    required property var model
                    readonly property bool isToday: model.today
                    readonly property bool inMonth: model.month === root.month

                    implicitWidth: 36
                    implicitHeight: 34

                    Rectangle {
                        anchors.centerIn: parent
                        width: 30
                        height: 30
                        radius: 15
                        color: cell.isToday ? Theme.palette.accent
                             : dayHover.hovered && cell.inMonth ? Theme.palette.tertiaryFill : "transparent"
                    }

                    StyledText {
                        anchors.centerIn: parent
                        text: cell.model.day
                        font.pointSize: Theme.font.size.small
                        font.weight: cell.isToday ? Font.DemiBold : Font.Normal
                        color: cell.isToday ? Theme.palette.labelOnAccent
                             : !cell.inMonth ? Theme.palette.tertiaryLabel
                             : Theme.palette.label
                        opacity: cell.inMonth || cell.isToday ? 1 : 0.6
                    }

                    HoverHandler {
                        id: dayHover
                    }
                }
            }
        }
    }
}
