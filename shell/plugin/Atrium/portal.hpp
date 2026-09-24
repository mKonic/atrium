#pragma once
// atrium's own xdg-desktop-portal backend (org.freedesktop.impl.portal.
// desktop.atrium), the atrium-portal program (shell/portal), started when
// the portal first asks for it. GlobalShortcuts: an app's shortcuts are rows of atrium's
// shortcut list (action "portal", arg "APP/ID"), so they're kept and can
// be changed in Settings; the compositor says when one is pressed and let go.

#include <QDBusAbstractAdaptor>
#include <QDBusArgument>
#include <QDBusContext>
#include <QDBusObjectPath>
#include <QHash>
#include <QObject>
#include <QVariantMap>

namespace atrium {

// One shortcut as the portal passes them: its id and {description, preferred_trigger}.
struct PortalShortcut {
    QString id;
    QVariantMap options;
};
using PortalShortcuts = QList<PortalShortcut>;

// The Settings portal's accent colour: red, green, blue, 0 to 1 (out of
// that: no preference).
struct PortalColor {
    double r = -1, g = -1, b = -1;
};
using PortalNamespaces = QMap<QString, QVariantMap>;

QDBusArgument& operator<<(QDBusArgument& arg, const PortalColor& c);
const QDBusArgument& operator>>(const QDBusArgument& arg, PortalColor& c);
QDBusArgument& operator<<(QDBusArgument& arg, const PortalShortcut& s);
const QDBusArgument& operator>>(const QDBusArgument& arg, PortalShortcut& s);

// A session the portal made for an app: its object, closed by either side.
class PortalSession : public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.impl.portal.Session")
    Q_PROPERTY(uint version READ version CONSTANT)

public:
    PortalSession(const QString& path, const QString& app, QObject* parent);
    ~PortalSession() override;

    uint version() const { return 1; }
    QString path() const { return path_; }
    QString app() const { return app_; }
    QStringList ids;  // the shortcuts it bound
    QHash<QString, QString> descriptions;  // id → what the app calls it

public slots:
    void Close();

signals:
    void Closed();

private:
    QString path_, app_;
};

class PortalBackend : public QObject, protected QDBusContext {
    Q_OBJECT

public:
    // An error for the D-Bus call being answered (adaptors have no context of their own).
    void fail(const QString& name, const QString& message) {
        if (calledFromDBus())
            sendErrorReply(name, message);
    }

    static PortalBackend* instance();

    // Takes the bus name; false when another backend has it.
    Q_INVOKABLE bool start();

    PortalSession* session(const QString& path) const { return sessions_.value(path); }
    void addSession(PortalSession* s);
    void dropSession(const QString& path) { sessions_.remove(path); }
    // The app's shortcuts as atrium has them: [{id, keys, row}] by id.
    QHash<QString, QVariantMap> shortcutsOf(const QString& app) const;
    // "Ctrl+Shift+A" from "ctrl+shift+a".
    static QString describe(const QString& keys);

signals:
    void pressed(const QString& app, const QString& id, bool down);

private:
    PortalBackend();
    QHash<QString, PortalSession*> sessions_;
};

class GlobalShortcutsAdaptor : public QDBusAbstractAdaptor {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.impl.portal.GlobalShortcuts")
    Q_PROPERTY(uint version READ version CONSTANT)

public:
    explicit GlobalShortcutsAdaptor(PortalBackend* parent);
    uint version() const { return 2; }

public slots:
    uint CreateSession(const QDBusObjectPath& handle, const QDBusObjectPath& session, const QString& app,
                       const QVariantMap& options, QVariantMap& results);
    uint BindShortcuts(const QDBusObjectPath& handle, const QDBusObjectPath& session,
                       const atrium::PortalShortcuts& shortcuts, const QString& parentWindow,
                       const QVariantMap& options, QVariantMap& results);
    uint ListShortcuts(const QDBusObjectPath& handle, const QDBusObjectPath& session, QVariantMap& results);
    void ConfigureShortcuts(const QDBusObjectPath& session, const QString& parentWindow, const QVariantMap& options);

signals:
    void Activated(const QDBusObjectPath& session, const QString& id, qulonglong timestamp, const QVariantMap& options);
    void Deactivated(const QDBusObjectPath& session, const QString& id, qulonglong timestamp,
                     const QVariantMap& options);
    void ShortcutsChanged(const QDBusObjectPath& session, const atrium::PortalShortcuts& shortcuts);

private:
    PortalShortcuts listFor(PortalSession* s) const;
    PortalBackend* backend_;
};

// Settings: dark or light, the accent and contrast, from atrium's settings,
// live (org.freedesktop.appearance).
class SettingsAdaptor : public QDBusAbstractAdaptor {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.impl.portal.Settings")
    Q_PROPERTY(uint version READ version CONSTANT)

public:
    explicit SettingsAdaptor(PortalBackend* parent);
    uint version() const { return 2; }

public slots:
    atrium::PortalNamespaces ReadAll(const QStringList& namespaces);
    QDBusVariant Read(const QString& ns, const QString& key);

signals:
    void SettingChanged(const QString& ns, const QString& key, const QDBusVariant& value);

private:
    QVariantMap appearance() const;
    QVariantMap last_;
};

} // namespace atrium

Q_DECLARE_METATYPE(atrium::PortalColor)
Q_DECLARE_METATYPE(atrium::PortalNamespaces)
Q_DECLARE_METATYPE(atrium::PortalShortcut)
Q_DECLARE_METATYPE(atrium::PortalShortcuts)
