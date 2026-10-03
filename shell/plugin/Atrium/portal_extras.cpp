// gio before Qt: Qt's `signals` macro breaks its headers.
#include <gio/gio.h>
#include <gio/gdesktopappinfo.h>

#include "portal_extras.hpp"
#include "portal_core.hpp"

#include <QCryptographicHash>
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusReply>
#include <QDBusUnixFileDescriptor>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <QUrl>

#include <pwd.h>
#include <unistd.h>


namespace atrium {

// --- GNOME settings ------------------------------------------------------------

namespace gnome_settings {

namespace {

// What GTK reads from the Settings portal (the schemas
// xdg-desktop-portal-gtk passed on), where installed.
constexpr const char* kSchemas[] = {
    "org.gnome.desktop.interface",   "org.gnome.desktop.wm.preferences", "org.gnome.desktop.a11y",
    "org.gnome.desktop.a11y.interface", "org.gnome.desktop.privacy",     "org.gnome.desktop.sound",
    "org.gnome.desktop.calendar",    "org.gnome.desktop.input-sources",  "org.gnome.desktop.peripherals.mouse",
    "org.gnome.desktop.lockdown",
};

bool installed(const char* schema) {
    GSettingsSchemaSource* source = g_settings_schema_source_get_default();
    GSettingsSchema* s = source ? g_settings_schema_source_lookup(source, schema, true) : nullptr;
    if (s)
        g_settings_schema_unref(s);
    return s != nullptr;
}

// The types D-Bus clients of the portal read; others are left out.
QVariant toQt(GVariant* v) {
    if (g_variant_is_of_type(v, G_VARIANT_TYPE_BOOLEAN))
        return bool(g_variant_get_boolean(v));
    if (g_variant_is_of_type(v, G_VARIANT_TYPE_STRING))
        return QString::fromUtf8(g_variant_get_string(v, nullptr));
    if (g_variant_is_of_type(v, G_VARIANT_TYPE_INT32))
        return int(g_variant_get_int32(v));
    if (g_variant_is_of_type(v, G_VARIANT_TYPE_UINT32))
        return uint(g_variant_get_uint32(v));
    if (g_variant_is_of_type(v, G_VARIANT_TYPE_INT64))
        return qlonglong(g_variant_get_int64(v));
    if (g_variant_is_of_type(v, G_VARIANT_TYPE_UINT64))
        return qulonglong(g_variant_get_uint64(v));
    if (g_variant_is_of_type(v, G_VARIANT_TYPE_DOUBLE))
        return g_variant_get_double(v);
    if (g_variant_is_of_type(v, G_VARIANT_TYPE_STRING_ARRAY)) {
        QStringList out;
        gsize n = 0;
        const gchar** strv = g_variant_get_strv(v, &n);
        for (gsize i = 0; i < n; ++i)
            out << QString::fromUtf8(strv[i]);
        g_free(strv);
        return out;
    }
    return {};
}

struct Watched {
    GSettings* settings = nullptr;
    QString ns;
    std::function<void(const QString&, const QString&, const QVariant&)> changed;
};

} // namespace

QStringList namespaces() {
    QStringList out;
    for (const char* s : kSchemas)
        if (installed(s))
            out << QString::fromLatin1(s);
    return out;
}

QVariantMap read(const QString& ns) {
    QVariantMap out;
    const QByteArray schema = ns.toLatin1();
    if (!installed(schema.constData()))
        return out;
    GSettings* settings = g_settings_new(schema.constData());
    GSettingsSchema* s = nullptr;
    g_object_get(settings, "settings-schema", &s, nullptr);
    gchar** keys = g_settings_schema_list_keys(s);
    for (gchar** k = keys; k && *k; ++k) {
        GVariant* v = g_settings_get_value(settings, *k);
        if (const QVariant q = toQt(v); q.isValid())
            out.insert(QString::fromUtf8(*k), q);
        g_variant_unref(v);
    }
    g_strfreev(keys);
    g_settings_schema_unref(s);
    g_object_unref(settings);
    return out;
}

void watch(std::function<void(const QString&, const QString&, const QVariant&)> changed) {
    // For the program's life: the portal backend lives as long.
    for (const QString& ns : namespaces()) {
        auto* w = new Watched{g_settings_new(ns.toLatin1().constData()), ns, changed};
        g_signal_connect(w->settings, "changed", G_CALLBACK(+[](GSettings* s, const gchar* key, gpointer data) {
                             auto* w = static_cast<Watched*>(data);
                             GVariant* v = g_settings_get_value(s, key);
                             if (const QVariant q = toQt(v); q.isValid())
                                 w->changed(w->ns, QString::fromUtf8(key), q);
                             g_variant_unref(v);
                         }),
                         w);
    }
}

} // namespace gnome_settings

// --- Email ---------------------------------------------------------------------

uint EmailAdaptor::ComposeEmail(const QDBusObjectPath&, const QString&, const QString&, const QVariantMap& options,
                                QVariantMap&) {
    auto list = [&](const char* key) {
        std::vector<std::string> out;
        for (const QString& a : options.value(key).toStringList())
            out.push_back(a.toStdString());
        return out;
    };
    portal::Email e{.to = list("addresses"), .cc = list("cc"), .bcc = list("bcc"),
                    .subject = options.value("subject").toString().toStdString(),
                    .body = options.value("body").toString().toStdString(),
                    .has_subject = options.contains("subject"), .has_body = options.contains("body")};
    if (options.contains("address"))
        e.to.insert(e.to.begin(), options.value("address").toString().toStdString());
    // Fails (2) when no app handles mailto:, as the app should then hear.
    GError* error = nullptr;
    const bool ok = g_app_info_launch_default_for_uri(portal::mailto(e).c_str(), nullptr, &error);
    if (error) {
        qWarning("atrium-portal: no mail app: %s", error->message);
        g_error_free(error);
    }
    return ok ? 0 : 2;
}

// --- Account -------------------------------------------------------------------

uint AccountAdaptor::GetUserInformation(const QDBusObjectPath& handle, const QString& app, const QString&,
                                        const QVariantMap& options, QVariantMap&) {
    const passwd* pw = getpwuid(getuid());
    const QString user = pw ? QString::fromLocal8Bit(pw->pw_name) : QString();
    QString name = pw ? QString::fromLocal8Bit(pw->pw_gecos).section(',', 0, 0) : QString();
    if (name.isEmpty())
        name = user;
    // The picture Settings → Users set (AccountsService), else ~/.face.
    QString image;
    for (const QString& f : {"/var/lib/AccountsService/icons/" + user, QDir::homePath() + "/.face"})
        if (QFileInfo(f).isReadable()) {
            image = QUrl::fromLocalFile(f).toString();
            break;
        }
    const QString reason = options.value("reason").toString();
    const QJsonObject question{
        {"app", app},
        {"title", "Share your details?"},
        {"subtitle", "The app is asking for your name and picture."},
        {"body", reason.isEmpty() ? QString() : "It says: " + reason},
        {"icon", "account_circle"},
        {"grant", "Share"},
        {"deny", "Don't Share"},
    };
    askShell(handle.path(), "access.qml", QJsonDocument(question).toJson(QJsonDocument::Compact), {},
             [user, name, image](const std::optional<QByteArray>& out) -> QVariantList {
                 const QJsonObject reply = out ? QJsonDocument::fromJson(*out).object() : QJsonObject();
                 const uint response = uint(reply.value("response").toInt(2));
                 if (response != 0)
                     return {response, QVariantMap()};
                 return {uint(0), QVariantMap{{"id", user}, {"name", name}, {"image", image}}};
             });
    return 2;
}

// --- DynamicLauncher -------------------------------------------------------------

uint DynamicLauncherAdaptor::PrepareInstall(const QDBusObjectPath& handle, const QString& app, const QString&,
                                            const QString& name, const QDBusVariant& icon, const QVariantMap& options,
                                            QVariantMap&) {
    const bool web = options.value("launcher_type").toUInt() == 2;
    const QJsonObject question{
        {"app", app},
        {"title", QString("Add “%1” to your apps?").arg(name)},
        {"subtitle", web ? "It opens the website as an app, from the launcher and the Dock."
                         : "It opens from the launcher and the Dock."},
        {"icon", "apps"},
        {"grant", "Add"},
        {"deny", "Cancel"},
    };
    const QDBusVariant keep = icon;
    askShell(handle.path(), "access.qml", QJsonDocument(question).toJson(QJsonDocument::Compact), {},
             [name, keep](const std::optional<QByteArray>& out) -> QVariantList {
                 const QJsonObject reply = out ? QJsonDocument::fromJson(*out).object() : QJsonObject();
                 const uint response = uint(reply.value("response").toInt(2));
                 if (response != 0)
                     return {response, QVariantMap()};
                 return {uint(0), QVariantMap{{"name", name}, {"icon", QVariant::fromValue(keep)}}};
             });
    return 2;
}

uint DynamicLauncherAdaptor::RequestInstallToken(const QString&, const QVariantMap&) {
    // Installing without asking is for software centres; everyone asks.
    return 2;
}

// --- Notification ------------------------------------------------------------------

namespace {

const QString kNotifications = QStringLiteral("org.freedesktop.Notifications");
const QString kNotificationsPath = QStringLiteral("/org/freedesktop/Notifications");

// A notification's icon as the server takes it: a theme name, or a file
// (pictures sent as bytes or a file descriptor are kept in the runtime dir).
QString iconOf(const QVariant& v) {
    if (!v.canConvert<QDBusVariant>() && !v.canConvert<QDBusArgument>())
        return {};
    const QDBusArgument arg =
        v.canConvert<QDBusVariant>() ? v.value<QDBusVariant>().variant().value<QDBusArgument>() : v.value<QDBusArgument>();
    QString kind;
    QDBusVariant value;
    arg.beginStructure();
    arg >> kind >> value;
    arg.endStructure();
    const QVariant inner = value.variant();
    if (kind == "themed") {
        const QStringList names = qdbus_cast<QStringList>(inner);
        return names.value(0);
    }
    QByteArray bytes;
    if (kind == "bytes")
        bytes = qdbus_cast<QByteArray>(inner);
    else if (kind == "file") {
        QFile f;
        const QDBusUnixFileDescriptor fd = qdbus_cast<QDBusUnixFileDescriptor>(inner);
        if (fd.isValid() && f.open(fd.fileDescriptor(), QIODevice::ReadOnly))
            bytes = f.read(16 << 20);
    }
    if (bytes.isEmpty())
        return {};
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation) + "/atrium-portal-icons";
    QDir().mkpath(dir);
    const QString file = dir + "/" + QCryptographicHash::hash(bytes, QCryptographicHash::Sha1).toHex().left(16);
    if (QFile out(file); !out.exists() && out.open(QIODevice::WriteOnly))
        out.write(bytes);
    return file;
}

// An a{sv} value as it is: a nested variant unwrapped, else itself.
QVariant plain(const QVariant& v) {
    return v.metaType() == QMetaType::fromType<QDBusVariant>() ? v.value<QDBusVariant>().variant() : v;
}

// The app's name from its desktop file, else its id.
QString nameOf(const QString& app) {
    QString name = app;
    if (GDesktopAppInfo* info = g_desktop_app_info_new((app + ".desktop").toUtf8().constData())) {
        name = QString::fromUtf8(g_app_info_get_name(G_APP_INFO(info)));
        g_object_unref(info);
    }
    return name;
}

QList<QVariantMap> buttonsOf(const QVariant& v) {
    QList<QVariantMap> out;
    if (!v.canConvert<QDBusArgument>())
        return out;
    const QDBusArgument arg = v.value<QDBusArgument>();
    arg.beginArray();
    while (!arg.atEnd()) {
        QVariantMap b;
        arg >> b;
        out << b;
    }
    arg.endArray();
    return out;
}

} // namespace

NotificationAdaptor::NotificationAdaptor(PortalBackend* parent) : QDBusAbstractAdaptor(parent) {
    QDBusConnection bus = QDBusConnection::sessionBus();
    bus.connect(kNotifications, kNotificationsPath, kNotifications, "ActionInvoked", this,
                SLOT(serverAction(uint, QString)));
    bus.connect(kNotifications, kNotificationsPath, kNotifications, "NotificationClosed", this,
                SLOT(serverClosed(uint, uint)));
}

uint NotificationAdaptor::serverIdOf(const QString& app, const QString& id) const {
    for (auto it = shown_.begin(); it != shown_.end(); ++it)
        if (it->app == app && it->id == id)
            return it.key();
    return 0;
}

void NotificationAdaptor::AddNotification(const QString& app, const QString& id, const QVariantMap& n) {
    Shown shown{app, id, {}};
    QStringList actions;
    // A click is the default action; buttons go by their index.
    if (n.contains("default-action")) {
        actions << "default" << "";
        shown.actions.insert("default", {n.value("default-action").toString(),
                                         plain(n.value("default-action-target"))});
    }
    const QList<QVariantMap> buttons = buttonsOf(n.value("buttons"));
    for (int i = 0; i < buttons.size(); ++i) {
        const QString key = "button-" + QString::number(i);
        actions << key << buttons[i].value("label").toString();
        shown.actions.insert(key, {buttons[i].value("action").toString(),
                                   plain(buttons[i].value("target"))});
    }
    const QString priority = n.value("priority").toString();
    QVariantMap hints{{"desktop-entry", app},
                      {"urgency", QVariant::fromValue(uchar(portal::urgency(priority.toStdString())))}};
    if (n.contains("category"))
        hints.insert("category", n.value("category"));
    QDBusMessage call = QDBusMessage::createMethodCall(kNotifications, kNotificationsPath, kNotifications, "Notify");
    call << nameOf(app) << serverIdOf(app, id) << iconOf(n.value("icon")) << n.value("title").toString()
         << n.value("body").toString() << actions << hints << int(-1);
    const QDBusReply<uint> reply = QDBusConnection::sessionBus().call(call, QDBus::Block, 5000);
    if (!reply.isValid()) {
        qWarning("atrium-portal: notification not shown: %s", qPrintable(reply.error().message()));
        return;
    }
    if (const uint old = serverIdOf(app, id); old && old != reply.value())
        shown_.remove(old);
    shown_.insert(reply.value(), shown);
}

void NotificationAdaptor::RemoveNotification(const QString& app, const QString& id) {
    const uint serverId = serverIdOf(app, id);
    if (!serverId)
        return;
    shown_.remove(serverId);
    QDBusMessage call =
        QDBusMessage::createMethodCall(kNotifications, kNotificationsPath, kNotifications, "CloseNotification");
    call << serverId;
    QDBusConnection::sessionBus().call(call, QDBus::NoBlock);
}

void NotificationAdaptor::serverAction(uint serverId, const QString& key) {
    auto it = shown_.find(serverId);
    if (it == shown_.end())
        return;
    const auto action = it->actions.value(key);
    if (action.first.isEmpty())
        return;
    QVariantList parameter;
    if (action.second.isValid())
        parameter << QVariant::fromValue(QDBusVariant(action.second));
    emit ActionInvoked(it->app, it->id, action.first, parameter);
}

void NotificationAdaptor::serverClosed(uint serverId, uint) {
    shown_.remove(serverId);
}

} // namespace atrium
