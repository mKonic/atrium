#include "portal_remote.hpp"

#include "compositor.hpp"
#include "portal_extras.hpp"

#include <QDBusArgument>
#include <QDBusConnection>
#include <QJsonDocument>
#include <QJsonObject>

#include <libei.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstring>

namespace atrium {

namespace {

const QString kVendor = QStringLiteral("atrium");

// A connection to a session's socket, for an app to drive libei over itself.
int connectTo(const QString& path) {
    const QByteArray p = path.toLocal8Bit();
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (size_t(p.size()) >= sizeof addr.sun_path)
        return -1;
    std::memcpy(addr.sun_path, p.constData(), size_t(p.size()));
    const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -1;
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

QString devicesText(uint devices) {
    QStringList what;
    if (devices & 1)
        what << "keyboard";
    if (devices & 2)
        what << "mouse";
    if (devices & 4)
        what << "touchscreen";
    if (what.size() > 1)
        what.last() = "and " + what.last();
    return what.join(what.size() > 2 ? ", " : " ");
}

} // namespace

// --- EiSender --------------------------------------------------------------------

EiSender::EiSender(const QString& path, QObject* parent) : QObject(parent) {
    ei_ = ei_new_sender(nullptr);
    ei_configure_name(ei_, "atrium-portal");
    if (ei_setup_backend_socket(ei_, path.toLocal8Bit().constData()) != 0) {
        ei_unref(ei_);
        ei_ = nullptr;
        return;
    }
    notifier_ = new QSocketNotifier(ei_get_fd(ei_), QSocketNotifier::Read, this);
    connect(notifier_, &QSocketNotifier::activated, this, &EiSender::dispatch);
    dispatch();
}

EiSender::~EiSender() {
    for (ei_touch* t : std::as_const(touches_))
        ei_touch_unref(t);
    for (ei_device* d : std::as_const(devices_))
        ei_device_unref(d);
    if (ei_)
        ei_unref(ei_);
}

void EiSender::dispatch() {
    ei_dispatch(ei_);
    while (ei_event* e = ei_get_event(ei_)) {
        switch (ei_event_get_type(e)) {
        case EI_EVENT_SEAT_ADDED:
            // Whatever the session allows.
            ei_seat_bind_capabilities(ei_event_get_seat(e), EI_DEVICE_CAP_POINTER, EI_DEVICE_CAP_POINTER_ABSOLUTE,
                                      EI_DEVICE_CAP_BUTTON, EI_DEVICE_CAP_SCROLL, EI_DEVICE_CAP_KEYBOARD,
                                      EI_DEVICE_CAP_TEXT, EI_DEVICE_CAP_TOUCH, nullptr);
            break;
        case EI_EVENT_DEVICE_RESUMED: {
            ei_device* d = ei_event_get_device(e);
            if (!devices_.contains(d))
                devices_.append(ei_device_ref(d));
            ei_device_start_emulating(d, sequence_++);
            flush();
            break;
        }
        case EI_EVENT_DEVICE_REMOVED: {
            ei_device* d = ei_event_get_device(e);
            if (devices_.removeOne(d))
                ei_device_unref(d);
            break;
        }
        default:
            break;
        }
        ei_event_unref(e);
    }
}

ei_device* EiSender::device(int cap) const {
    for (ei_device* d : devices_)
        if (ei_device_has_capability(d, ei_device_capability(cap)))
            return d;
    return nullptr;
}

void EiSender::frame(ei_device* d) {
    ei_device_frame(d, ei_now(ei_));
}

void EiSender::motion(double dx, double dy) {
    with(EI_DEVICE_CAP_POINTER, [dx, dy](ei_device* d) { ei_device_pointer_motion(d, dx, dy); });
}

void EiSender::motionAbsolute(double x, double y) {
    with(EI_DEVICE_CAP_POINTER_ABSOLUTE, [x, y](ei_device* d) { ei_device_pointer_motion_absolute(d, x, y); });
}

void EiSender::button(uint32_t button, bool press) {
    with(EI_DEVICE_CAP_BUTTON, [button, press](ei_device* d) { ei_device_button_button(d, button, press); });
}

void EiSender::scroll(double dx, double dy, bool finish) {
    with(EI_DEVICE_CAP_SCROLL, [dx, dy, finish](ei_device* d) {
        if (dx != 0 || dy != 0)
            ei_device_scroll_delta(d, dx, dy);
        if (finish)
            ei_device_scroll_stop(d, true, true);
    });
}

void EiSender::scrollDiscrete(int32_t dx120, int32_t dy120) {
    with(EI_DEVICE_CAP_SCROLL, [dx120, dy120](ei_device* d) { ei_device_scroll_discrete(d, dx120, dy120); });
}

void EiSender::key(uint32_t keycode, bool press) {
    with(EI_DEVICE_CAP_KEYBOARD, [keycode, press](ei_device* d) { ei_device_keyboard_key(d, keycode, press); });
}

void EiSender::keysym(uint32_t sym, bool press) {
    with(EI_DEVICE_CAP_TEXT, [sym, press](ei_device* d) { ei_device_text_keysym(d, sym, press); });
}

void EiSender::touchDown(uint32_t slot, double x, double y) {
    with(EI_DEVICE_CAP_TOUCH, [this, slot, x, y](ei_device* d) {
        if (touches_.contains(slot))
            return;
        ei_touch* t = ei_device_touch_new(d);
        ei_touch_down(t, x, y);
        touches_.insert(slot, t);
    });
}

void EiSender::touchMotion(uint32_t slot, double x, double y) {
    with(EI_DEVICE_CAP_TOUCH, [this, slot, x, y](ei_device*) {
        if (ei_touch* t = touches_.value(slot))
            ei_touch_motion(t, x, y);
    });
}

void EiSender::touchUp(uint32_t slot) {
    with(EI_DEVICE_CAP_TOUCH, [this, slot](ei_device*) {
        if (ei_touch* t = touches_.take(slot)) {
            ei_touch_up(t);
            ei_touch_unref(t);
        }
    });
}

void EiSender::with(int cap, std::function<void(ei_device*)> send) {
    if (waiting_.size() >= 1024)
        return;  // a device that never came: not kept forever
    waiting_.append({cap, std::move(send)});
    flush();
}

void EiSender::flush() {
    while (!waiting_.isEmpty()) {
        ei_device* d = device(waiting_.first().first);
        if (!d)
            return;  // not there yet: when it is
        waiting_.takeFirst().second(d);
        frame(d);
    }
}

// --- RemoteDesktop --------------------------------------------------------------

RemoteDesktopAdaptor::RemoteDesktopAdaptor(PortalBackend* parent, ScreenCastAdaptor* screencast)
    : QDBusAbstractAdaptor(parent), screencast_(screencast) {
    // atrium going takes every session's remote input with it.
    connect(Compositor::instance(), &Compositor::connectedChanged, this, [this] {
        if (Compositor::instance()->connected())
            return;
        for (const QString& path : remotes_.keys())
            if (PortalSession* s = PortalBackend::instance()->session(path))
                s->end();
    });
}

void RemoteDesktopAdaptor::end(const QString& session) {
    const Remote r = remotes_.take(session);
    delete r.sender.data();
    if (r.eis)
        Compositor::instance()->eisClose(r.eis);
}

uint RemoteDesktopAdaptor::CreateSession(const QDBusObjectPath&, const QDBusObjectPath& session, const QString& app,
                                         const QVariantMap&, QVariantMap& results) {
    auto* s = new PortalSession(session.path(), app, PortalBackend::instance());
    PortalBackend::instance()->addSession(s);
    remotes_.insert(session.path(), Remote{});
    // It may share the screen too.
    screencast_->adopt(session.path());
    const QString path = session.path();
    connect(s, &QObject::destroyed, this, [this, path] { end(path); });
    results.insert("session_id", path);
    return 0;
}

uint RemoteDesktopAdaptor::SelectDevices(const QDBusObjectPath&, const QDBusObjectPath& session, const QString&,
                                         const QVariantMap& options, QVariantMap&) {
    auto it = remotes_.find(session.path());
    if (it == remotes_.end() || it->started)
        return 2;
    if (options.contains("types"))
        it->devices = options.value("types").toUInt() & deviceTypes();
    if (!it->devices)
        it->devices = deviceTypes();
    it->persist = options.value("persist_mode").toUInt();
    if (options.contains("restore_data")) {
        PortalRestore r;
        options.value("restore_data").value<QDBusArgument>() >> r;
        const QVariantMap data = qdbus_cast<QVariantMap>(r.data.variant());
        // Allowed before for these devices (or more): not asked again.
        if (r.vendor == kVendor && r.version == 1 && (data.value("devices").toUInt() & it->devices) == it->devices) {
            it->restored = true;
            screencast_->restoreFrom(session.path(), data, it->persist);
        }
    }
    return 0;
}

uint RemoteDesktopAdaptor::Start(const QDBusObjectPath& handle, const QDBusObjectPath& session, const QString& app,
                                 const QString&, const QVariantMap&, QVariantMap&) {
    auto it = remotes_.find(session.path());
    if (it == remotes_.end() || it->started)
        return 2;
    const QString path = session.path();
    PortalBackend* backend = PortalBackend::instance();
    const QDBusMessage call = backend->delayReply();
    auto* request = new PortalRequest(handle.path(), backend);
    auto answer = [call, request](uint response, const QVariantMap& results) {
        if (request->property("answered").toBool())
            return;
        request->setProperty("answered", true);
        QDBusConnection::sessionBus().send(call.createReply(QVariantList{response, results}));
        request->deleteLater();
    };

    // Allowed: its remote input socket, then the screen if it asked for one.
    auto go = [this, path, request, answer] {
        auto r = remotes_.find(path);
        if (r == remotes_.end())
            return answer(2, {});
        Compositor::instance()->eisOpen(r->devices, [this, path, request, answer](const QJsonObject& reply) {
            auto r = remotes_.find(path);
            if (r == remotes_.end() || !reply.value("ok").toBool())
                return answer(2, {});
            const QJsonObject res = reply.value("result").toObject();
            r->eis = res.value("session").toInteger();
            r->path = res.value("path").toString();
            r->started = true;
            // Connected now, ready for its first Notify.
            r->sender = new EiSender(r->path, this);
            const uint devices = r->devices;
            const uint persist = r->persist;
            auto finish = [devices, persist, answer](uint response, QVariantMap results) {
                if (response != 0)
                    return answer(response, {});
                results.insert("devices", devices);
                if (persist) {
                    // Allowed again without asking: these devices, and the
                    // screen it showed, if any.
                    QVariantMap data{{"devices", devices}};
                    if (results.contains("restore_data")) {
                        PortalRestore cast;
                        results.value("restore_data").value<QDBusArgument>() >> cast;
                        data.insert(qdbus_cast<QVariantMap>(cast.data.variant()));
                    }
                    const PortalRestore restore{kVendor, 1, QDBusVariant(data)};
                    results.insert("persist_mode", persist);
                    results.insert("restore_data", QVariant::fromValue(restore));
                }
                answer(0, results);
            };
            if (screencast_->selected(path)) {
                screencast_->restoreFrom(path, {}, persist);  // so the screen's pick comes back to keep
                screencast_->startCast(path, request, [finish](uint response, const QVariantMap& results) {
                    finish(response, results);
                });
            } else {
                finish(0, {});
            }
        });
    };

    if (it->restored)
        return go(), 2;
    // Asked first, as for any access.
    const QString name = appDisplayName(app);
    const QJsonObject question{
        {"app", app},
        {"title", app.isEmpty() ? QString("Allow an app to control this computer?")
                                : QString("Allow “%1” to control this computer?").arg(name)},
        {"subtitle", QString()},
        {"body", QString("It can use the %1 as if it were you, until you stop it.").arg(devicesText(it->devices))},
        {"icon", "preferences-desktop-remote-desktop"},
        {"grant", "Allow"},
        {"deny", "Don't Allow"},
    };
    auto* dialog = new QProcess(request);
    QObject::connect(request, &PortalRequest::closed, dialog, [dialog] { dialog->kill(); });
    QObject::connect(dialog, &QProcess::finished, request, [dialog, go, answer](int code, QProcess::ExitStatus st) {
        const QJsonObject reply = QJsonDocument::fromJson(dialog->readAllStandardOutput()).object();
        if (st != QProcess::NormalExit || code != 0 || reply.value("response").toInt(2) != 0)
            return answer(1, {});
        go();
    });
    QObject::connect(dialog, &QProcess::errorOccurred, request, [answer](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart)
            answer(2, {});
    });
    dialog->start(shellProgram(), {shellFile("access.qml")});
    dialog->write(QJsonDocument(question).toJson(QJsonDocument::Compact));
    dialog->closeWriteChannel();
    return 2;  // unused: the reply goes later
}

RemoteDesktopAdaptor::Remote* RemoteDesktopAdaptor::started(const QDBusObjectPath& session, uint device) {
    auto it = remotes_.find(session.path());
    if (it == remotes_.end() || !it->started || !(it->devices & device))
        return nullptr;
    // The portal's own connection, made when first needed.
    if (!it->sender) {
        it->sender = new EiSender(it->path, this);
        if (!it->sender->ok()) {
            delete it->sender.data();
            return nullptr;
        }
    }
    return &*it;
}

QPointF RemoteDesktopAdaptor::onDesktop(const QDBusObjectPath& session, uint stream, double x, double y) const {
    const QPoint origin = screencast_->origin(session.path(), stream).value_or(QPoint());
    return {origin.x() + x, origin.y() + y};
}

void RemoteDesktopAdaptor::NotifyPointerMotion(const QDBusObjectPath& session, const QVariantMap&, double dx,
                                               double dy) {
    if (Remote* r = started(session, 2))
        r->sender->motion(dx, dy);
}

void RemoteDesktopAdaptor::NotifyPointerMotionAbsolute(const QDBusObjectPath& session, const QVariantMap&,
                                                       uint stream, double x, double y) {
    if (Remote* r = started(session, 2)) {
        const QPointF p = onDesktop(session, stream, x, y);
        r->sender->motionAbsolute(p.x(), p.y());
    }
}

void RemoteDesktopAdaptor::NotifyPointerButton(const QDBusObjectPath& session, const QVariantMap&, int button,
                                               uint state) {
    if (Remote* r = started(session, 2))
        r->sender->button(uint32_t(button), state != 0);
}

void RemoteDesktopAdaptor::NotifyPointerAxis(const QDBusObjectPath& session, const QVariantMap& options, double dx,
                                             double dy) {
    if (Remote* r = started(session, 2))
        r->sender->scroll(dx, dy, options.value("finish").toBool());
}

void RemoteDesktopAdaptor::NotifyPointerAxisDiscrete(const QDBusObjectPath& session, const QVariantMap&, uint axis,
                                                     int steps) {
    if (Remote* r = started(session, 2))
        r->sender->scrollDiscrete(axis == 1 ? steps * 120 : 0, axis == 0 ? steps * 120 : 0);
}

void RemoteDesktopAdaptor::NotifyKeyboardKeycode(const QDBusObjectPath& session, const QVariantMap&, int keycode,
                                                 uint state) {
    if (Remote* r = started(session, 1))
        r->sender->key(uint32_t(keycode), state != 0);
}

void RemoteDesktopAdaptor::NotifyKeyboardKeysym(const QDBusObjectPath& session, const QVariantMap&, int keysym,
                                                uint state) {
    if (Remote* r = started(session, 1))
        r->sender->keysym(uint32_t(keysym), state != 0);
}

void RemoteDesktopAdaptor::NotifyTouchDown(const QDBusObjectPath& session, const QVariantMap&, uint stream, uint slot,
                                           double x, double y) {
    if (Remote* r = started(session, 4)) {
        const QPointF p = onDesktop(session, stream, x, y);
        r->sender->touchDown(slot, p.x(), p.y());
    }
}

void RemoteDesktopAdaptor::NotifyTouchMotion(const QDBusObjectPath& session, const QVariantMap&, uint stream,
                                             uint slot, double x, double y) {
    if (Remote* r = started(session, 4)) {
        const QPointF p = onDesktop(session, stream, x, y);
        r->sender->touchMotion(slot, p.x(), p.y());
    }
}

void RemoteDesktopAdaptor::NotifyTouchUp(const QDBusObjectPath& session, const QVariantMap&, uint slot) {
    if (Remote* r = started(session, 4))
        r->sender->touchUp(slot);
}

QDBusUnixFileDescriptor RemoteDesktopAdaptor::ConnectToEIS(const QDBusObjectPath& session, const QString&,
                                                           const QVariantMap&) {
    auto it = remotes_.find(session.path());
    if (it == remotes_.end() || !it->started)
        return {};
    const int fd = connectTo(it->path);
    if (fd < 0)
        return {};
    QDBusUnixFileDescriptor out(fd);  // a dup
    ::close(fd);
    return out;
}

} // namespace atrium
