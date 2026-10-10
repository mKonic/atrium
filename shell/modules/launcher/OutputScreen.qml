import QtQuick
import shell.components
import shell.services
import Atrium

// What a custom command (or a line run in the shell) printed, as it prints it.
Item {
    id: screen

    required property LauncherModel launcherModel
    readonly property var hints: launcherModel.outputRunning ? [["Stop", "Ctrl+C"]] : [["Run Again", "Ctrl+R"], ["Copy", "Ctrl+Shift+C"]]

    function handleKey(event: var): bool {
        const ctrl = event.modifiers & Qt.ControlModifier;
        if (ctrl && event.key === Qt.Key_C && !(event.modifiers & Qt.ShiftModifier) && launcherModel.outputRunning)
            launcherModel.stopOutput();
        else if (ctrl && event.key === Qt.Key_R && !launcherModel.outputRunning)
            launcherModel.rerunOutput();
        else if (ctrl && (event.modifiers & Qt.ShiftModifier) && event.key === Qt.Key_C)
            launcherModel.copyOutput();
        else if (event.key === Qt.Key_Down)
            flick.contentY = Math.min(flick.contentHeight - flick.height, flick.contentY + 40);
        else if (event.key === Qt.Key_Up)
            flick.contentY = Math.max(0, flick.contentY - 40);
        else
            return false;
        return true;
    }

    Row {
        id: status

        anchors.top: parent.top
        anchors.left: parent.left
        anchors.margins: 16
        spacing: 8

        MaterialIcon {
            anchors.verticalCenter: parent.verticalCenter
            text: screen.launcherModel.outputRunning ? "progress_activity" : screen.launcherModel.outputExit === 0 ? "check_circle" : "error"
            color: screen.launcherModel.outputRunning ? Theme.palette.secondaryLabel
                 : screen.launcherModel.outputExit === 0 ? Theme.palette.green : Theme.palette.red

            RotationAnimation on rotation {
                running: screen.launcherModel.outputRunning
                from: 0
                to: 360
                duration: 1000
                loops: Animation.Infinite
            }
        }

        StyledText {
            anchors.verticalCenter: parent.verticalCenter
            text: screen.launcherModel.outputRunning ? qsTr("Running") : screen.launcherModel.outputExit === 0 ? qsTr("Finished") : qsTr("Exited with %1").arg(screen.launcherModel.outputExit)
            font.pointSize: Theme.font.size.smaller
            color: Theme.palette.secondaryLabel
        }
    }

    Flickable {
        id: flick

        anchors.top: status.bottom
        anchors.topMargin: 10
        anchors.bottom: parent.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.leftMargin: 16
        anchors.rightMargin: 16
        clip: true
        contentHeight: text.implicitHeight + 12
        boundsBehavior: Flickable.StopAtBounds
        // Follows the end while it prints, unless scrolled up.
        property bool following: true
        onContentHeightChanged: if (following) contentY = Math.max(0, contentHeight - height)
        onMovementEnded: following = contentY >= contentHeight - height - 4

        TextEdit {
            id: text

            width: flick.width
            readOnly: true
            selectByMouse: true
            text: screen.launcherModel.output
            wrapMode: Text.Wrap
            color: Theme.palette.label
            selectionColor: Theme.palette.accent
            selectedTextColor: Theme.palette.labelOnAccent
            font.family: Theme.font.mono
            font.pointSize: Theme.font.size.smaller
            textFormat: TextEdit.PlainText
        }
    }
}
