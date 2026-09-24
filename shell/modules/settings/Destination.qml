import QtQuick
import shell.components
import shell.services
import Atrium

// Where an app's windows open: anywhere, a numbered space, or a secret one.
// Reports changes as registry fields ({ space, secret }).
Row {
    id: root

    property int space: 0
    property string secret: ""
    signal changed(var fields)

    readonly property string kind: secret ? "secret" : space ? "space" : "any"
    // Secret spaces that exist or have apps, to pick from.
    readonly property var secretNames: {
        const names = new Set(["communication"]);
        for (const s of Atrium.spaces)
            if (s.secret)
                names.add(s.label);
        for (const a of Atrium.apps)
            if (a.secret)
                names.add(a.secret);
        return [...names];
    }

    spacing: 8

    Dropdown {
        fieldWidth: 150
        value: root.kind
        options: [
            { value: "any", label: "Anywhere" },
            { value: "space", label: "A space" },
            { value: "secret", label: "A secret space" }
        ]
        onPicked: v => {
            if (v === "any")
                root.changed({ space: 0, secret: "" });
            else if (v === "space")
                root.changed({ space: 1, secret: "" });
            else
                root.changed({ space: 0, secret: root.secretNames[0] });
        }
    }

    Dropdown {
        visible: root.kind === "space"
        fieldWidth: 110
        value: String(root.space)
        options: [1, 2, 3, 4, 5, 6, 7, 8, 9].map(n => ({ value: String(n), label: `Space ${n}` }))
        onPicked: v => root.changed({ space: Number(v), secret: "" })
    }

    Dropdown {
        visible: root.kind === "secret"
        fieldWidth: 160
        value: root.secret
        options: root.secretNames.map(n => ({ value: n, label: n.charAt(0).toUpperCase() + n.slice(1) }))
        onPicked: v => root.changed({ space: 0, secret: v })
    }
}
