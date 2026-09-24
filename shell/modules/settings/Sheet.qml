import QtQuick
import QtQuick.Controls
import shell.components
import shell.services

// A macOS-style sheet over the window: a title, a few fields, a line for
// what went wrong, and Cancel and the action. Fields go in as children.
Popup {
    id: root

    property string title
    property string action: "OK"
    property string error: ""
    property bool busy: false
    property bool ready: true
    default property alias fields: body.data
    signal submitted

    parent: Overlay.overlay
    anchors.centerIn: parent
    modal: true
    focus: true
    padding: 24
    width: 380
    closePolicy: busy ? Popup.NoAutoClose : Popup.CloseOnEscape

    onOpened: error = ""

    Overlay.modal: Rectangle {
        color: Theme.palette.scrim
    }

    background: Rectangle {
        radius: 16
        color: Theme.palette.windowBackground
        border.width: 1
        border.color: Theme.palette.separator
    }

    contentItem: Column {
        spacing: 14

        StyledText {
            text: root.title
            font.pointSize: Theme.font.size.larger
            font.weight: Font.DemiBold
        }

        Column {
            id: body

            width: parent.width
            spacing: 10
        }

        StyledText {
            visible: root.error !== ""
            width: parent.width
            wrapMode: Text.WordWrap
            text: root.error
            font.pointSize: Theme.font.size.smaller
            color: Theme.palette.red
        }

        Row {
            anchors.right: parent.right
            spacing: 8

            PillButton {
                text: "Cancel"
                onClicked: if (!root.busy) root.close()
            }

            PillButton {
                text: root.busy ? "Working…" : root.action
                primary: true
                opacity: root.ready && !root.busy ? 1 : 0.5
                onClicked: if (root.ready && !root.busy) root.submitted()
            }
        }
    }
}
