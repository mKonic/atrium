#pragma once
// The InputCapture portal (Deskflow, Input Leap): an app sharing one
// keyboard and mouse between computers sets barriers on the desktop's
// outside edges; once the pointer goes through one, atrium sends the
// pointer and keys to the app (over libei) instead, until it hands them
// back. Asked once, when the app starts a session.

#include "portal.hpp"

#include <QDBusAbstractAdaptor>
#include <QDBusUnixFileDescriptor>
#include <QHash>

namespace atrium {

// A zone as the portal lists them: (uuii) width, height, x, y.
struct PortalZone {
    uint width = 0, height = 0;
    int x = 0, y = 0;
};
using PortalZones = QList<PortalZone>;
QDBusArgument& operator<<(QDBusArgument& arg, const PortalZone& z);
const QDBusArgument& operator>>(const QDBusArgument& arg, PortalZone& z);

class InputCaptureAdaptor : public QDBusAbstractAdaptor {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.impl.portal.InputCapture")
    Q_PROPERTY(uint SupportedCapabilities READ capabilities CONSTANT)
    Q_PROPERTY(uint version READ version CONSTANT)

public:
    explicit InputCaptureAdaptor(PortalBackend* parent);
    uint capabilities() const { return 1 | 2; }  // keyboard, pointer
    uint version() const { return 1; }

public slots:
    uint CreateSession(const QDBusObjectPath& handle, const QDBusObjectPath& session, const QString& app,
                       const QString& parentWindow, const QVariantMap& options, QVariantMap& results);
    uint GetZones(const QDBusObjectPath& handle, const QDBusObjectPath& session, const QString& app,
                  const QVariantMap& options, QVariantMap& results);
    uint SetPointerBarriers(const QDBusObjectPath& handle, const QDBusObjectPath& session, const QString& app,
                            const QVariantMap& options, const QList<QVariantMap>& barriers, uint zoneSet,
                            QVariantMap& results);
    uint Enable(const QDBusObjectPath& session, const QString& app, const QVariantMap& options, QVariantMap& results);
    uint Disable(const QDBusObjectPath& session, const QString& app, const QVariantMap& options,
                 QVariantMap& results);
    uint Release(const QDBusObjectPath& session, const QString& app, const QVariantMap& options,
                 QVariantMap& results);
    QDBusUnixFileDescriptor ConnectToEIS(const QDBusObjectPath& session, const QString& app,
                                         const QVariantMap& options);

private:
    struct Capture {
        qint64 id = 0;  // atrium's session
        QString path;   // its socket
    };
    void signal(const QString& session, const QString& name, const QVariantMap& options);
    void end(const QString& session);
    QString sessionOf(qint64 id) const;

    QHash<QString, Capture> captures_;  // by session path
    uint zoneSet_ = 1;
};

} // namespace atrium

Q_DECLARE_METATYPE(atrium::PortalZone)
Q_DECLARE_METATYPE(atrium::PortalZones)
