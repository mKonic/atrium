#pragma once
// Secret: a sandboxed app's own key (Flatpak Chrome's, say), kept in the
// keyring (atrium-keyring, through the Secret Service) and handed over on a
// pipe. The same attributes KWallet's portal used, so a key it made carries
// over: {server: xdg-desktop-portal, type: binary, user: the app's id}.

#include "portal.hpp"

#include <QDBusAbstractAdaptor>
#include <QDBusObjectPath>
#include <QDBusUnixFileDescriptor>
#include <QDBusVariant>

#include <functional>

namespace atrium {

// Waits for a Secret Service prompt's Completed (D-Bus signals reach slots).
class SecretPromptWaiter : public QObject {
    Q_OBJECT

public:
    explicit SecretPromptWaiter(std::function<void(bool)> then) : then_(std::move(then)) {}

public slots:
    void completed(bool dismissed, const QDBusVariant&) {
        then_(!dismissed);
        deleteLater();
    }

private:
    std::function<void(bool)> then_;
};

class SecretAdaptor : public QDBusAbstractAdaptor {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.impl.portal.Secret")
    Q_PROPERTY(uint version READ version CONSTANT)

public:
    explicit SecretAdaptor(PortalBackend* parent) : QDBusAbstractAdaptor(parent) {}
    uint version() const { return 1; }

public slots:
    uint RetrieveSecret(const QDBusObjectPath& handle, const QString& app, const QDBusUnixFileDescriptor& fd,
                        const QVariantMap& options, QVariantMap& results);
};

} // namespace atrium
