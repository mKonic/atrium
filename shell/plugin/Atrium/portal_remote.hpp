#pragma once
// The RemoteDesktop portal: an app (a remote-desktop server, a KVM switch,
// a test runner) asks to control the keyboard, pointer and touch, and maybe
// to see the screen too. Once the user allows it, atrium opens a remote
// input socket (libei) for the session: the app gets a connection to it
// (ConnectToEIS), or sends through the portal's Notify* calls, which the
// portal passes on over a connection of its own.

#include "portal.hpp"

#include <QDBusAbstractAdaptor>
#include <QDBusUnixFileDescriptor>
#include <QHash>
#include <QPointer>
#include <QSocketNotifier>

#include <functional>

struct ei;
struct ei_device;
struct ei_touch;

namespace atrium {

// The portal's own libei connection to a session's socket, sending what
// the app asks through D-Bus.
class EiSender : public QObject {
    Q_OBJECT

public:
    EiSender(const QString& path, QObject* parent);
    ~EiSender() override;
    bool ok() const { return ei_ != nullptr; }

    void motion(double dx, double dy);
    void motionAbsolute(double x, double y);
    void button(uint32_t button, bool press);
    void scroll(double dx, double dy, bool finish);
    void scrollDiscrete(int32_t dx120, int32_t dy120);
    void key(uint32_t keycode, bool press);
    void keysym(uint32_t sym, bool press);
    void touchDown(uint32_t slot, double x, double y);
    void touchMotion(uint32_t slot, double x, double y);
    void touchUp(uint32_t slot);

private:
    void dispatch();
    // The device that has `cap` (an ei_device_capability), or null.
    ei_device* device(int cap) const;
    void frame(ei_device* d);
    // `send` with the device for `cap`: now, or (just connected) once it's
    // there, in the order asked.
    void with(int cap, std::function<void(ei_device*)> send);
    void flush();

    ::ei* ei_ = nullptr;
    QSocketNotifier* notifier_ = nullptr;
    QList<ei_device*> devices_;
    QHash<uint32_t, ei_touch*> touches_;
    QList<std::pair<int, std::function<void(ei_device*)>>> waiting_;
    uint32_t sequence_ = 1;
};

class RemoteDesktopAdaptor : public QDBusAbstractAdaptor {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.impl.portal.RemoteDesktop")
    Q_PROPERTY(uint AvailableDeviceTypes READ deviceTypes CONSTANT)
    Q_PROPERTY(uint version READ version CONSTANT)

public:
    RemoteDesktopAdaptor(PortalBackend* parent, ScreenCastAdaptor* screencast);
    uint deviceTypes() const { return 1 | 2 | 4; }  // keyboard, pointer, touchscreen
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
    void NotifyPointerAxisDiscrete(const QDBusObjectPath& session, const QVariantMap& options, uint axis,
                                   int steps);
    void NotifyKeyboardKeycode(const QDBusObjectPath& session, const QVariantMap& options, int keycode, uint state);
    void NotifyKeyboardKeysym(const QDBusObjectPath& session, const QVariantMap& options, int keysym, uint state);
    void NotifyTouchDown(const QDBusObjectPath& session, const QVariantMap& options, uint stream, uint slot,
                         double x, double y);
    void NotifyTouchMotion(const QDBusObjectPath& session, const QVariantMap& options, uint stream, uint slot,
                           double x, double y);
    void NotifyTouchUp(const QDBusObjectPath& session, const QVariantMap& options, uint slot);
    QDBusUnixFileDescriptor ConnectToEIS(const QDBusObjectPath& session, const QString& app,
                                         const QVariantMap& options);

private:
    struct Remote {
        uint devices = 1 | 2 | 4;
        uint persist = 0;
        bool restored = false;  // allowed before: not asked again
        bool started = false;
        qint64 eis = 0;         // atrium's session
        QString path;           // its socket
        QPointer<EiSender> sender;
    };
    Remote* started(const QDBusObjectPath& session, uint device);
    // Where a stream's (x, y) is on the desktop.
    QPointF onDesktop(const QDBusObjectPath& session, uint stream, double x, double y) const;
    void end(const QString& session);

    ScreenCastAdaptor* screencast_;
    QHash<QString, Remote> remotes_;  // by session path
};

} // namespace atrium
