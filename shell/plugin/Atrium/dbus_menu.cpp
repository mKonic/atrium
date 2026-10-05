#include "dbus_menu.hpp"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCall>
#include <QDBusVariant>
#include <QDateTime>
#include <QVariantMap>

namespace atrium::dbusmenu {

namespace {

const QString kMenu = QStringLiteral("com.canonical.dbusmenu");

QVariant plain(const QVariant& v) {
    return v.canConvert<QDBusVariant>() ? v.value<QDBusVariant>().variant() : v;
}

// dbusmenu marks mnemonics with "_" ("__" is a literal one); the shell
// shows none.
QString plain_label(QString label) {
    label.replace("__", QString(QChar(1)));
    label.remove('_');
    label.replace(QChar(1), "_");
    return label;
}

QVariantList entries_of(const Node& node) {
    QVariantList out;
    for (const Node& c : node.children) {
        if (c.props.contains("visible") && !plain(c.props.value("visible")).toBool())
            continue;
        if (plain(c.props.value("type")).toString() == "separator") {
            out.append(QVariantMap{{"separator", true}});
            continue;
        }
        const bool sub = !c.children.isEmpty() || plain(c.props.value("children-display")).toString() == "submenu";
        const QString toggle = plain(c.props.value("toggle-type")).toString();
        const bool ticked = (toggle == "checkmark" || toggle == "radio") && plain(c.props.value("toggle-state")).toInt() == 1;
        out.append(QVariantMap{
            {"id", c.id},
            {"text", plain_label(plain(c.props.value("label")).toString())},
            {"checked", ticked},
            {"enabled", !c.props.contains("enabled") || plain(c.props.value("enabled")).toBool()},
            {"separator", false},
            {"shortcut", shortcutText(c.props.value("shortcut"))},
            {"submenu", sub},
            {"children", sub ? entries_of(c) : QVariantList()},
        });
    }
    return out;
}

} // namespace

QString shortcutText(const QVariant& shortcut) {
    const QVariant v = plain(shortcut);
    QList<QStringList> chords;
    if (v.metaType() == QMetaType::fromType<QDBusArgument>())
        v.value<QDBusArgument>() >> chords;
    else
        for (const QVariant& c : v.toList())
            chords.append(c.toStringList());
    if (chords.isEmpty())
        return {};
    // The first chord, as a Mac writes it: modifiers as symbols, then the key.
    QString text;
    for (const QString& part : chords.first()) {
        if (part == "Control")
            text += QString::fromUtf8("⌃");
        else if (part == "Alt")
            text += QString::fromUtf8("⌥");
        else if (part == "Shift")
            text += QString::fromUtf8("⇧");
        else if (part == "Super")
            text += QString::fromUtf8("⌘");
        else
            text += part.size() == 1 ? part.toUpper() : part;
    }
    return text;
}

Node readNode(const QDBusArgument& arg) {
    Node n;
    arg.beginStructure();
    arg >> n.id >> n.props;
    arg.beginArray();
    while (!arg.atEnd()) {
        QDBusVariant v;
        arg >> v;
        n.children.append(readNode(v.variant().value<QDBusArgument>()));
    }
    arg.endArray();
    arg.endStructure();
    return n;
}

QVariantList entries(const Node& node) {
    return entries_of(node);
}

QVariantList layout(const QString& service, const QString& path, int parent) {
    if (service.isEmpty() || path.isEmpty() || path == "/")
        return {};
    QDBusConnection bus = QDBusConnection::sessionBus();
    // Apps that build their menu lazily do it now.
    QDBusMessage about = QDBusMessage::createMethodCall(service, path, kMenu, "AboutToShow");
    about << parent;
    bus.call(about, QDBus::Block, 500);
    QDBusMessage get = QDBusMessage::createMethodCall(service, path, kMenu, "GetLayout");
    get << parent << -1 << QStringList();
    const QDBusMessage reply = bus.call(get, QDBus::Block, 1000);
    if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().size() < 2)
        return {};
    return entries(readNode(reply.arguments().at(1).value<QDBusArgument>()));
}

void trigger(const QString& service, const QString& path, int id) {
    QDBusMessage m = QDBusMessage::createMethodCall(service, path, kMenu, "Event");
    m << id << QStringLiteral("clicked") << QVariant::fromValue(QDBusVariant(0))
      << uint(QDateTime::currentSecsSinceEpoch());
    QDBusConnection::sessionBus().asyncCall(m);
}

} // namespace atrium::dbusmenu
