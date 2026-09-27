import QtQuick
import Atrium.Shell
import shell.components
import shell.services
import Atrium

// What a palette action did where it can't be seen ("Trash Emptied"), for a
// moment at the bottom of the screen, like the OSD out of the way of the pointer.
PanelWindow {
    id: pill

    property bool shown: false
    property string glyph
    property string text
    property bool noop: false

    function show(glyph: string, text: string, noop: bool): void {
        pill.glyph = glyph;
        pill.text = text;
        pill.noop = noop;
        shown = true;
        hideTimer.restart();
    }

    screen: Shell.screen(Atrium.focusedOutput?.name)
    visible: shown || card.opacity > 0
    anchors.bottom: true
    margins.bottom: 110
    implicitWidth: card.width
    implicitHeight: 44
    exclusiveZone: 0
    color: "transparent"
    mask: Region {}
    WlrLayershell.layer: WlrLayer.Overlay
    WlrLayershell.namespace: "atrium-osd"

    Timer {
        id: hideTimer

        interval: 1800
        onTriggered: pill.shown = false
    }

    Rectangle {
        id: card

        width: row.implicitWidth + 36
        height: parent.height
        radius: height / 2
        color: Theme.material.regular

        Glass {}

        border.width: Theme.lens ? 0 : 1
        border.color: Theme.palette.separator
        opacity: pill.shown ? 1 : 0
        scale: pill.shown ? 1 : 0.94

        Behavior on opacity {
            Anim {}
        }
        Behavior on scale {
            Anim {}
        }

        Row {
            id: row

            anchors.centerIn: parent
            spacing: 8

            StyledText {
                anchors.verticalCenter: parent.verticalCenter
                text: pill.text
                font.weight: Font.Medium
            }

            // Changed something, or found nothing to do.
            MaterialIcon {
                anchors.verticalCenter: parent.verticalCenter
                text: pill.noop ? "info" : "check_circle"
                color: pill.noop ? Theme.palette.secondaryLabel : Theme.palette.green
            }
        }
    }
}
