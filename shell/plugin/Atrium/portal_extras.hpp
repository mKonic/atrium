#pragma once
// The rest of what xdg-desktop-portal-gtk did for atrium: GNOME's settings
// (from GSettings) for the Settings portal, Email, Account, DynamicLauncher
// and Notification (on atrium's notification server). Lockdown needs no
// backend: without one the portal locks nothing down.

#include "portal.hpp"

#include <QDBusAbstractAdaptor>
#include <QDBusVariant>
#include <QHash>
#include <QStringList>
#include <QVariantMap>

#include <functional>

namespace atrium {

// GNOME's settings namespaces (org.gnome.desktop.interface, ...) as GTK
// apps in a sandbox read them, straight from GSettings.
namespace gnome_settings {
QStringList namespaces();  // those installed here
QVariantMap read(const QString& ns);
// `changed(ns, key, value)` whenever one of them changes.
void watch(std::function<void(const QString&, const QString&, const QVariant&)> changed);
} // namespace gnome_settings

// Email: the mail app, by a mailto: link.
class EmailAdaptor : public QDBusAbstractAdaptor {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.impl.portal.Email")
    Q_PROPERTY(uint version READ version CONSTANT)

public:
    explicit EmailAdaptor(PortalBackend* parent) : QDBusAbstractAdaptor(parent) {}
    uint version() const { return 4; }

public slots:
    uint ComposeEmail(const QDBusObjectPath& handle, const QString& app, const QString& window,
                      const QVariantMap& options, QVariantMap& results);
};

// Account: the user's name and picture, once they agree.
class AccountAdaptor : public QDBusAbstractAdaptor {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.impl.portal.Account")
    Q_PROPERTY(uint version READ version CONSTANT)

public:
    explicit AccountAdaptor(PortalBackend* parent) : QDBusAbstractAdaptor(parent) {}
    uint version() const { return 1; }

public slots:
    uint GetUserInformation(const QDBusObjectPath& handle, const QString& app, const QString& window,
                            const QVariantMap& options, QVariantMap& results);
};

// DynamicLauncher: an app adding a launcher (a web app) asks first.
class DynamicLauncherAdaptor : public QDBusAbstractAdaptor {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.impl.portal.DynamicLauncher")
    Q_PROPERTY(uint SupportedLauncherTypes READ launcherTypes CONSTANT)
    Q_PROPERTY(uint version READ version CONSTANT)

public:
    explicit DynamicLauncherAdaptor(PortalBackend* parent) : QDBusAbstractAdaptor(parent) {}
    uint launcherTypes() const { return 1 | 2; }  // applications, web apps
    uint version() const { return 1; }

public slots:
    uint PrepareInstall(const QDBusObjectPath& handle, const QString& app, const QString& window,
                        const QString& name, const QDBusVariant& icon, const QVariantMap& options,
                        QVariantMap& results);
    uint RequestInstallToken(const QString& app, const QVariantMap& options);
};

// Notification: a sandboxed app's notifications, shown by atrium's
// notification server (org.freedesktop.Notifications); its buttons and a
// click come back as ActionInvoked.
class NotificationAdaptor : public QDBusAbstractAdaptor {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.impl.portal.Notification")
    Q_PROPERTY(uint version READ version CONSTANT)
    Q_PROPERTY(QVariantMap SupportedOptions READ supportedOptions CONSTANT)

public:
    explicit NotificationAdaptor(PortalBackend* parent);
    uint version() const { return 2; }
    QVariantMap supportedOptions() const { return {}; }

public slots:
    void AddNotification(const QString& app, const QString& id, const QVariantMap& notification);
    void RemoveNotification(const QString& app, const QString& id);

signals:
    void ActionInvoked(const QString& app, const QString& id, const QString& action, const QVariantList& parameter);

private slots:
    void serverAction(uint serverId, const QString& key);
    void serverClosed(uint serverId, uint reason);

private:
    struct Shown {
        QString app, id;
        // Action keys sent to the server → the app's action and its target.
        QHash<QString, std::pair<QString, QVariant>> actions;
    };
    QHash<uint, Shown> shown_;  // by the server's id
    uint serverIdOf(const QString& app, const QString& id) const;
};

} // namespace atrium
