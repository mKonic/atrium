import QtQuick
import Atrium.Shell
import shell.components
import shell.services
import Atrium

// The focused app, as a macOS menu bar names it: in bold, then its title.
Item {
    id: root

    readonly property var window: Atrium.focusedWindow
    // With nothing in front, the desktop belongs to the file manager, as to
    // Finder on a Mac.
    readonly property string appName: window ? Icons.appName(window.app_id) : AppMenu.desktopApp.name ?? ""
    // Its menu is open (AppMenus): drawn pressed.
    property bool menuOpen: false
    signal clicked
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
    // The window's title after the app's name (not when the app's menus are there).
    property bool showTitle: true

    // Nothing focused (or no app): nothing shown, not an empty pill. (The
    // bar sets this item's own opacity, so the content fades instead.)
    readonly property bool shown: appName !== ""

    implicitHeight: Theme.bar.inner
    implicitWidth: shown ? row.implicitWidth + (Theme.lens ? Theme.padding.normal * 2 : 0) : 0

    MouseArea {
        id: nameArea

        anchors.fill: parent
        hoverEnabled: true
        enabled: root.shown
        z: 2
        onClicked: root.clicked()
    }

    // Liquid Glass: in a glass pill, so it reads over any wallpaper.
    Rectangle {
        anchors.fill: parent
        radius: height / 2
        visible: Theme.lens
        opacity: root.shown ? 1 : 0
        color: Theme.bar.pill

        Glass {}

        Behavior on opacity {
            Anim {}
        }
    }

    // Pressed while its menu is open, lit under the pointer.
    Rectangle {
        anchors.fill: parent
        anchors.leftMargin: Theme.lens ? 0 : -Theme.padding.normal
        anchors.rightMargin: Theme.lens ? 0 : -Theme.padding.normal
        radius: height / 2
        visible: root.shown
        color: root.menuOpen ? Theme.bar.selected : nameArea.containsMouse ? Theme.bar.palette.fill : "transparent"
    }

    Row {
        id: row

        x: Theme.lens ? Theme.padding.normal : 0
        anchors.verticalCenter: parent.verticalCenter
        opacity: root.shown ? 1 : 0
        spacing: Theme.spacing.small

        Behavior on opacity {
            Anim {}
        }

        StyledText {
            color: Theme.bar.palette.label
            anchors.verticalCenter: parent.verticalCenter
            text: root.appName
            font.weight: Font.Bold
        }

        StyledText {
            id: titleText

            anchors.verticalCenter: parent.verticalCenter
            width: Math.max(0, Math.min(implicitWidth, 420, root.maxWidth - x - (Theme.lens ? Theme.padding.normal * 2 : 0)))
            elide: Text.ElideRight
            text: root.title
            color: Theme.bar.palette.secondaryLabel
            visible: root.showTitle && text.length > 0 && width > 40
        }
    }
}
