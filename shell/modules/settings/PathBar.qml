pragma ComponentBehavior: Bound

import QtQuick
import shell.components
import shell.services
import Atrium

// Where a file picker is: an up button and the folders down to this one,
// each a click away; clicking beside them (or `type`) takes a typed address
// instead ("~/Pictures", "/usr/share", a file). `chose` says which.
Row {
    id: root

    required property FolderModel model
    readonly property bool editing: pathInput.visible
    signal chose(string path)
    signal done  // the address was left: focus can go back

    // Starts typing an address with `text` already there.
    function type(text: string): void {
        pathInput.text = text;
        pathInput.visible = true;
        pathInput.forceActiveFocus();
    }

    function stop(): void {
        pathInput.visible = false;
    }

    spacing: 8

    Rectangle {
        width: 32
        height: 32
        radius: 8
        color: upArea.containsMouse ? Theme.palette.secondaryFill : Theme.palette.tertiaryFill
        opacity: root.model.folder !== "/" ? 1 : 0.4

        MaterialIcon {
            anchors.centerIn: parent
            text: "arrow_upward"
            color: Theme.palette.label
        }

        MouseArea {
            id: upArea

            anchors.fill: parent
            hoverEnabled: true
            onClicked: root.model.up()
        }
    }

    Rectangle {
        width: root.width - 40
        height: 32
        radius: 8
        color: Theme.palette.tertiaryFill
        border.width: 1
        border.color: pathInput.activeFocus ? Theme.palette.focusRing : "transparent"
        clip: true

        MouseArea {
            anchors.fill: parent
            cursorShape: Qt.IBeamCursor
            onClicked: root.type(root.model.folder === "/" ? "/" : root.model.folder + "/")
        }

        // The end of a long path in view: where it is matters most.
        Row {
            visible: !root.editing
            x: Math.min(6, parent.width - width - 6)
            anchors.verticalCenter: parent.verticalCenter

            Repeater {
                model: root.model.crumbs

                Row {
                    id: crumb

                    required property var modelData
                    required property int index
                    readonly property bool last: index === root.model.crumbs.length - 1

                    MaterialIcon {
                        visible: crumb.index > 0
                        anchors.verticalCenter: parent.verticalCenter
                        text: "chevron_right"
                        font.pointSize: Theme.font.size.small
                        color: Theme.palette.tertiaryLabel
                    }

                    Rectangle {
                        width: crumbText.implicitWidth + 12
                        height: 24
                        radius: 6
                        color: crumbArea.containsMouse ? Theme.palette.secondaryFill : "transparent"

                        StyledText {
                            id: crumbText

                            anchors.centerIn: parent
                            text: crumb.index === 0 ? "Computer" : crumb.modelData.name
                            font.pointSize: Theme.font.size.small
                            font.weight: crumb.last ? Font.DemiBold : Font.Normal
                            color: crumb.last ? Theme.palette.label : Theme.palette.secondaryLabel
                        }

                        MouseArea {
                            id: crumbArea

                            anchors.fill: parent
                            hoverEnabled: true
                            onClicked: root.chose(crumb.modelData.path)
                        }
                    }
                }
            }
        }

        TextInput {
            id: pathInput

            selectionColor: Theme.palette.accent
            selectedTextColor: Theme.palette.labelOnAccent
            visible: false
            anchors.fill: parent
            anchors.leftMargin: 10
            anchors.rightMargin: 10
            verticalAlignment: TextInput.AlignVCenter
            color: Theme.palette.label
            font.family: Theme.font.sans
            font.pointSize: Theme.font.size.small
            selectByMouse: true
            onAccepted: root.chose(text)
            onActiveFocusChanged: if (!activeFocus) visible = false
            Keys.onEscapePressed: {
                visible = false;
                root.done();
            }
        }
    }
}
