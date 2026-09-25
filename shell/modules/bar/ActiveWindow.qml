import QtQuick
import Atrium.Shell
import shell.components
import shell.services
import Atrium

// The focused app, as a macOS menu bar names it: its icon and title.
Item {
    id: root

    readonly property var window: Atrium.focusedWindow
    readonly property string appName: window ? Icons.appName(window.app_id) : ""
    // Titles often end in the app's name ("notes — Kate"); the name is already shown.
    readonly property string title: {
        const t = window?.title ?? "";
        for (const sep of [" — ", " - ", " – ", " | "])
            if (t.endsWith(sep + appName))
                return t.slice(0, -(sep.length + appName.length));
        return t === appName ? "" : t;
    }

    // The room there is before the bar's right side; the title gives way first.
    property real maxWidth: 10000

    implicitHeight: Theme.bar.inner
    implicitWidth: row.implicitWidth + (Theme.lens ? Theme.padding.normal * 2 : 0)
    opacity: window ? 1 : 0

    // Liquid Glass: in a glass pill, so it reads over any wallpaper.
    Rectangle {
        anchors.fill: parent
        radius: height / 2
        visible: Theme.lens
        color: Theme.material.pill

        Glass {}
    }

    Behavior on opacity {
        Anim {}
    }

    Row {
        id: row

        x: Theme.lens ? Theme.padding.normal : 0
        anchors.verticalCenter: parent.verticalCenter
        spacing: Theme.spacing.small

        IconImage {
            anchors.verticalCenter: parent.verticalCenter
            implicitSize: 18
            source: root.window ? Icons.appIcon(root.window.app_id) : ""
            asynchronous: true
        }

        StyledText {
            anchors.verticalCenter: parent.verticalCenter
            text: root.appName
            font.weight: Font.DemiBold
        }

        StyledText {
            id: titleText

            anchors.verticalCenter: parent.verticalCenter
            width: Math.max(0, Math.min(implicitWidth, 420, root.maxWidth - x - (Theme.lens ? Theme.padding.normal * 2 : 0)))
            elide: Text.ElideRight
            text: root.title
            color: Theme.palette.secondaryLabel
            visible: text.length > 0 && width > 40
        }
    }
}
