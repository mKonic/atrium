pragma ComponentBehavior: Bound

import QtQuick
import Atrium.Shell
import shell.components
import shell.services
import Atrium

// Every app atrium keeps something for: where its windows open, whether
// showing its secret space starts it, its place in the Dock, how it opens,
// and where it was.
Column {
    id: root

    property string expanded: ""  // the app whose details are open
    property bool picking: false

    spacing: 12

    function nameOf(id: string): string {
        return DesktopEntries.heuristicLookup(id)?.name ?? id;
    }

    function iconOf(id: string): string {
        return Shell.iconPath(DesktopEntries.heuristicLookup(id)?.icon ?? id, "application-x-executable");
    }

    SectionHeader {
        width: parent.width
        subtitle: "Choose where an app's windows open, keep it in the Dock, or start it with its secret space."

        PillButton {
            text: root.picking ? "Cancel" : "Add App"
            icon: root.picking ? "" : "add"
            primary: !root.picking
            onClicked: {
                root.picking = !root.picking;
                search.text = "";
            }
        }
    }

    // Picking an app to add: search the installed ones.
    Rectangle {
        visible: root.picking
        width: parent.width
        height: pickColumn.implicitHeight + 24
        radius: 14
        color: Theme.palette.tertiaryFill
        border.width: 1
        border.color: Theme.palette.separator

        LauncherResults {
            id: found

            appsOnly: true
            entries: DesktopEntries
            query: search.text
        }

        Column {
            id: pickColumn

            x: 12
            y: 12
            width: parent.width - 24
            spacing: 4

            Rectangle {
                width: parent.width
                height: 34
                radius: 9
                color: Theme.palette.tertiaryFill

                TextInput {
                    id: search

                    anchors.fill: parent
                    anchors.leftMargin: 12
                    anchors.rightMargin: 12
                    verticalAlignment: TextInput.AlignVCenter
                    color: Theme.palette.label
                    font.family: Theme.font.sans
                    font.pointSize: Theme.font.size.normal
                    focus: root.picking
                    clip: true

                    StyledText {
                        anchors.verticalCenter: parent.verticalCenter
                        visible: !search.text
                        text: "Search apps"
                        color: Theme.palette.tertiaryLabel
                    }
                }
            }

            Repeater {
                model: found

                Rectangle {
                    id: hit

                    required property string title
                    required property string icon
                    required property var appId

                    width: pickColumn.width
                    height: 40
                    radius: 9
                    color: hitArea.containsMouse ? Theme.palette.tertiaryFill : "transparent"

                    IconImage {
                        id: hitIcon

                        x: 8
                        anchors.verticalCenter: parent.verticalCenter
                        implicitSize: 26
                        source: Shell.iconPath(hit.icon, "application-x-executable")
                    }

                    StyledText {
                        anchors.left: hitIcon.right
                        anchors.leftMargin: 10
                        anchors.verticalCenter: parent.verticalCenter
                        text: hit.title
                    }

                    MouseArea {
                        id: hitArea

                        anchors.fill: parent
                        hoverEnabled: true
                        onClicked: {
                            // A new record starts in the Dock, the one thing
                            // every app can have.
                            Atrium.setPinned(hit.appId, true);
                            root.expanded = hit.appId;
                            root.picking = false;
                        }
                    }
                }
            }
        }
    }

    Rectangle {
        width: parent.width
        height: list.implicitHeight
        radius: 14
        color: Theme.palette.tertiaryFill
        border.width: 1
        border.color: Theme.palette.separator

        Column {
            id: list

            width: parent.width

            StyledText {
                visible: Atrium.apps.length === 0
                padding: 16
                text: "No apps yet."
                color: Theme.palette.secondaryLabel
            }

            Repeater {
                model: Atrium.apps

                Column {
                    id: app

                    required property var modelData
                    required property int index
                    readonly property bool open: root.expanded === modelData.app_id
                    readonly property string summary: {
                        const parts = [];
                        if (modelData.secret)
                            parts.push(`Opens in ${modelData.secret}` + (modelData.launch ? ", started with it" : ""));
                        else if (modelData.space)
                            parts.push(`Opens in space ${modelData.space}`);
                        if (modelData.dock !== null)
                            parts.push("In the Dock");
                        if (modelData.fullscreen)
                            parts.push("Fullscreen");
                        else if (modelData.maximized)
                            parts.push("Maximized");
                        if (modelData.placement)
                            parts.push("Size remembered");
                        return parts.join(" · ");
                    }

                    width: list.width

                    Rectangle {
                        visible: app.index > 0
                        x: 16
                        width: parent.width - 32
                        height: 1
                        color: Theme.palette.separator
                    }

                    // The app, and what it has, at a glance.
                    Item {
                        width: parent.width
                        height: 56

                        IconImage {
                            id: appIcon

                            x: 16
                            anchors.verticalCenter: parent.verticalCenter
                            implicitSize: 32
                            source: root.iconOf(app.modelData.app_id)
                        }

                        Column {
                            anchors.left: appIcon.right
                            anchors.leftMargin: 12
                            anchors.right: chevron.left
                            anchors.verticalCenter: parent.verticalCenter

                            StyledText {
                                text: root.nameOf(app.modelData.app_id)
                                font.weight: Font.Medium
                            }

                            StyledText {
                                width: parent.width
                                text: app.summary
                                elide: Text.ElideRight
                                font.pointSize: Theme.font.size.small
                                color: Theme.palette.secondaryLabel
                            }
                        }

                        MaterialIcon {
                            id: chevron

                            anchors.right: parent.right
                            anchors.rightMargin: 16
                            anchors.verticalCenter: parent.verticalCenter
                            text: "chevron_right"
                            rotation: app.open ? 90 : 0
                            color: Theme.palette.secondaryLabel

                            Behavior on rotation {
                                Anim {
                                    duration: Theme.anim.small
                                }
                            }
                        }

                        MouseArea {
                            anchors.fill: parent
                            onClicked: root.expanded = app.open ? "" : app.modelData.app_id
                        }
                    }

                    // Its details.
                    Column {
                        visible: app.open
                        x: 60
                        width: parent.width - 76
                        spacing: 10
                        bottomPadding: 16

                        Detail {
                            label: "Opens in"

                            Destination {
                                space: app.modelData.space
                                secret: app.modelData.secret
                                onChanged: fields => Atrium.setApp(app.modelData.app_id, fields.secret ? fields : Object.assign(fields, { launch: "" }))
                            }
                        }

                        Detail {
                            visible: app.modelData.secret !== ""
                            label: "Start with the space"

                            Switch {
                                checked: app.modelData.launch !== ""
                                onToggled: Atrium.setApp(app.modelData.app_id, {
                                    launch: checked ? "" : (DesktopEntries.heuristicLookup(app.modelData.app_id)?.execString ?? app.modelData.app_id).replace(/ %[a-zA-Z]/g, "")
                                })
                            }

                            TextControl {
                                visible: app.modelData.launch !== ""
                                fieldWidth: 220
                                placeholder: "Command"
                                value: app.modelData.launch
                                onCommitted: v => Atrium.setApp(app.modelData.app_id, { launch: v })
                            }
                        }

                        Detail {
                            label: "Keep in the Dock"

                            Switch {
                                checked: app.modelData.dock !== null
                                onToggled: Atrium.setPinned(app.modelData.app_id, !checked)
                            }
                        }

                        Detail {
                            label: "Opens"

                            ChoiceControl {
                                value: app.modelData.fullscreen ? "fullscreen" : app.modelData.maximized ? "maximized" : "normal"
                                choices: ["normal", "maximized", "fullscreen"]
                                onPicked: v => Atrium.setApp(app.modelData.app_id, {
                                    maximized: v === "maximized" ? true : null,
                                    fullscreen: v === "fullscreen" ? true : null
                                })
                            }
                        }

                        Row {
                            spacing: 8

                            PillButton {
                                visible: !!app.modelData.placement
                                text: "Forget Its Size"
                                onClicked: Atrium.setApp(app.modelData.app_id, { placement: null })
                            }

                            PillButton {
                                text: "Forget App"
                                onClicked: Atrium.forgetApp(app.modelData.app_id)
                            }
                        }
                    }
                }
            }
        }
    }

    component Detail: Row {
        property string label
        default property alias controls: holder.data

        spacing: 12

        StyledText {
            anchors.verticalCenter: parent.verticalCenter
            width: 150
            text: parent.label
            font.pointSize: Theme.font.size.small
            color: Theme.palette.secondaryLabel
        }

        Row {
            id: holder

            anchors.verticalCenter: parent.verticalCenter
            spacing: 8
        }
    }
}
