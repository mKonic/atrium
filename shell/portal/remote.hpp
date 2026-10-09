#pragma once
// org.freedesktop.impl.portal.RemoteDesktop and .InputCapture: remote control
// and input capture, done by the compositor (src/eis.hpp) and asked for over
// a connection each session holds (IpcLink).
//
// RemoteDesktop: after the app says which devices it wants (and, on the same
// session, what screen to share), Start asks the user: atrium's chooser when
// a screen goes with it ("Choose what to share and control"), else an
// "allow this?" dialog. Input then comes through libei (ConnectToEIS) or the
// Notify* calls.
//
// InputCapture (Deskflow, Input Leap): Start asks the user; the app sets
// barriers on the screens' outer edges, and the pointer pushed through one
// is the app's until it releases it (or Super+Shift+Escape).

#include "ipc_link.hpp"

#include <QDBusAbstractAdaptor>
#include <QDBusArgument>
#include <QDBusObjectPath>
#include <QDBusUnixFileDescriptor>
#include <QPointer>
#include <QVariantMap>

#include <memory>
#include <optional>

namespace atrium {

class PortalBackend;
class PortalSession;

// A zone as GetZones answers it: (uuii) width, height, x, y.
struct CaptureZone {
    uint width = 0, height = 0;
    int x = 0, y = 0;
};
using CaptureZones = QList<CaptureZone>;
QDBusArgument& operator<<(QDBusArgument& arg, const CaptureZone& z);
const QDBusArgument& operator>>(const QDBusArgument& arg, CaptureZone& z);

// A session's link to the compositor's side of it, kept on its PortalSession.
class InputState : public QObject {
    Q_OBJECT

public:
    explicit InputState(PortalSession* session);
    static InputState* of(PortalSession* session);

    uint devices = 7;   // asked for: keyboard 1, pointer 2, touchscreen 4
    uint granted = 0;   // allowed, once started
    uint persist = 0;
    bool restored = false;
    std::unique_ptr<IpcLink> link;
    uint cookie = 0;
    bool capture = false;
    uint zone_set = 1;
};

class RemoteDesktopAdaptor : public QDBusAbstractAdaptor {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.impl.portal.RemoteDesktop")
    Q_PROPERTY(uint AvailableDeviceTypes READ deviceTypes CONSTANT)
    Q_PROPERTY(uint version READ version CONSTANT)

public:
    explicit RemoteDesktopAdaptor(PortalBackend* backend);
    uint deviceTypes() const { return 7; }
    uint version() const { return 2; }

public slots:
    uint CreateSession(const QDBusObjectPath& handle, const QDBusObjectPath& session, const QString& app,
                       const QVariantMap& options, QVariantMap& results);
    uint SelectDevices(const QDBusObjectPath& handle, const QDBusObjectPath& session, const QString& app,
                       const QVariantMap& options, QVariantMap& results);
    uint Start(const QDBusObjectPath& handle, const QDBusObjectPath& session, const QString& app,
               const QString& parentWindow, const QVariantMap& options, QVariantMap& results);
    void NotifyPointerMotion(const QDBusObjectPath& session, const QVariantMap& options, double dx, double dy);
    void NotifyPointerMotionAbsolute(const QDBusObjectPath& session, const QVariantMap& options, uint stream,
                                     double x, double y);
    void NotifyPointerButton(const QDBusObjectPath& session, const QVariantMap& options, int button, uint state);
    void NotifyPointerAxis(const QDBusObjectPath& session, const QVariantMap& options, double dx, double dy);
    void NotifyPointerAxisDiscrete(const QDBusObjectPath& session, const QVariantMap& options, uint axis, int steps);
    void NotifyKeyboardKeycode(const QDBusObjectPath& session, const QVariantMap& options, int keycode, uint state);
    void NotifyKeyboardKeysym(const QDBusObjectPath& session, const QVariantMap& options, int keysym, uint state);
    void NotifyTouchDown(const QDBusObjectPath& session, const QVariantMap& options, uint stream, uint slot,
                         double x, double y);
    void NotifyTouchMotion(const QDBusObjectPath& session, const QVariantMap& options, uint stream, uint slot,
                           double x, double y);
    void NotifyTouchUp(const QDBusObjectPath& session, const QVariantMap& options, uint slot);
    QDBusUnixFileDescriptor ConnectToEIS(const QDBusObjectPath& session, const QString& app, const QVariantMap& options);

private:
    void input(const QDBusObjectPath& session, QJsonObject event);
    // A stream's point as a point on the desktop.
    std::optional<QPointF> place(PortalSession* s, uint stream, double x, double y) const;
    PortalBackend* backend_;
};

class InputCaptureAdaptor : public QDBusAbstractAdaptor {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.impl.portal.InputCapture")
    Q_PROPERTY(uint SupportedCapabilities READ capabilities CONSTANT)
    Q_PROPERTY(uint version READ version CONSTANT)

public:
    explicit InputCaptureAdaptor(PortalBackend* backend);
    uint capabilities() const { return 3; }  // keyboard and pointer
    uint version() const { return 2; }

public slots:
    QVariantMap CreateSession2(const QDBusObjectPath& session, const QString& app, const QVariantMap& options);
    uint Start(const QDBusObjectPath& handle, const QDBusObjectPath& session, const QString& app,
               const QString& parentWindow, const QVariantMap& options, QVariantMap& results);
    uint GetZones(const QDBusObjectPath& handle, const QDBusObjectPath& session, const QString& app,
                  const QVariantMap& options, QVariantMap& results);
    uint SetPointerBarriers(const QDBusObjectPath& handle, const QDBusObjectPath& session, const QString& app,
                            const QVariantMap& options, const QList<QVariantMap>& barriers, uint zoneSet,
                            QVariantMap& results);
    uint Enable(const QDBusObjectPath& session, const QString& app, const QVariantMap& options, QVariantMap& results);
    uint Disable(const QDBusObjectPath& session, const QString& app, const QVariantMap& options, QVariantMap& results);
    uint Release(const QDBusObjectPath& session, const QString& app, const QVariantMap& options, QVariantMap& results);
    QDBusUnixFileDescriptor ConnectToEIS(const QDBusObjectPath& session, const QString& app, const QVariantMap& options);

signals:
    void Disabled(const QDBusObjectPath& session, const QVariantMap& options);
    void Activated(const QDBusObjectPath& session, const QVariantMap& options);
    void Deactivated(const QDBusObjectPath& session, const QVariantMap& options);
    void ZonesChanged(const QDBusObjectPath& session, const QVariantMap& options);

private:
    InputState* started(const QDBusObjectPath& session) const;
    void compositorEvent(const QJsonObject& e);
    PortalBackend* backend_;
};

} // namespace atrium

Q_DECLARE_METATYPE(atrium::CaptureZone)
