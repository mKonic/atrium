#pragma once
// com.canonical.dbusmenu, the menus apps put on D-Bus: a tray icon's, and
// a window's for the menu bar. As the shell shows them:
//   [{ id, text, checked, enabled, separator, shortcut, children }]
// (shortcut as macOS writes it, "⌃⇧S"; children a submenu's own).

#include <QDBusArgument>
#include <QString>
#include <QVariantList>
#include <QVariantMap>

namespace atrium::dbusmenu {

// `parent`'s children in the menu at `service`/`path` (0: the top), after
// AboutToShow, so apps that fill a menu as it opens have.
QVariantList layout(const QString& service, const QString& path, int parent = 0);
void trigger(const QString& service, const QString& path, int id);

// A menu item as GetLayout gives it: (ia{sv}av), its id, properties and children.
struct Node {
    int id = 0;
    QVariantMap props;
    QList<Node> children;
};
Node readNode(const QDBusArgument& layout);
// A node's children as entries.
QVariantList entries(const Node& node);
// A "shortcut" property (aas: [["Control", "Shift", "S"]]) as "⌃⇧S".
QString shortcutText(const QVariant& shortcut);

} // namespace atrium::dbusmenu
