pragma ComponentBehavior: Bound

import QtQuick
import shell.components
import shell.services

// How windows open, as an app record or a rule says it (Hyprland's window
// rules): where, whether you go with them, in what state, and how they sit
// among the others. Reports changes as registry fields.
Column {
    id: root

    property var record: ({})
    // An app can start with its secret space; a pattern rule has no one app to start.
    property bool canLaunch: false
    property string launchCommand: ""
    signal changed(var fields)

    readonly property bool placed: !!record.space || !!record.secret

    spacing: 10

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

    component Flag: Detail {
        id: flag

        property string field

        Switch {
            checked: root.record[flag.field] === true
            onToggled: root.changed({ [flag.field]: checked ? null : true })
        }
    }

    Detail {
        label: "Opens in"

        Destination {
            space: root.record.space ?? 0
            secret: root.record.secret ?? ""
            onChanged: fields => root.changed(fields.secret ? fields : Object.assign(fields, { launch: "", follow: null }))
        }
    }

    Flag {
        visible: root.placed
        label: "Go there with it"
        field: "follow"
    }

    Detail {
        visible: root.canLaunch && !!root.record.secret
        label: "Start with the space"

        Switch {
            checked: (root.record.launch ?? "") !== ""
            onToggled: root.changed({ launch: checked ? "" : root.launchCommand })
        }

        TextControl {
            visible: (root.record.launch ?? "") !== ""
            fieldWidth: 220
            placeholder: "Command"
            value: root.record.launch ?? ""
            onCommitted: v => root.changed({ launch: v })
        }
    }

    Detail {
        label: "Opens"

        ChoiceControl {
            value: root.record.fullscreen ? "fullscreen" : root.record.maximized ? "maximized" : "normal"
            choices: ["normal", "maximized", "fullscreen"]
            onPicked: v => root.changed({
                maximized: v === "maximized" ? true : null,
                fullscreen: v === "fullscreen" ? true : null
            })
        }
    }

    Flag {
        label: "Float when tiling"
        field: "floating"
    }

    Flag {
        label: "Keep above others"
        field: "keep_above"
    }

    Flag {
        label: "On every space"
        field: "sticky"
    }

    Flag {
        label: "Open without focus"
        field: "no_focus"
    }

    Flag {
        label: "Keep drawing out of sight"
        field: "render_unfocused"
    }
}
