#include "remote.hpp"

#include "compositor.hpp"
#include "portal.hpp"
#include "screencast.hpp"
#include "wayland_link.hpp"

#include <QDBusConnection>
#include <QDBusMetaType>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPointF>

#include <unistd.h>

namespace atrium {

QDBusArgument& operator<<(QDBusArgument& arg, const CaptureZone& z) {
    arg.beginStructure();
    arg << z.width << z.height << z.x << z.y;
    arg.endStructure();
    return arg;
}

const QDBusArgument& operator>>(const QDBusArgument& arg, CaptureZone& z) {
    arg.beginStructure();
    arg >> z.width >> z.height >> z.x >> z.y;
    arg.endStructure();
    return arg;
}

namespace {

// Asks the user with atrium's "allow this?" dialog, then answers the call.
void askAllow(const QString& handle, const QString& app, const QString& title, const QString& body,
              const QString& parentWindow, std::function<QVariantList(bool)> answer) {
    const QJsonObject question{
        {"app", app},
        {"title", title},
        {"body", body},
        {"icon", "input-keyboard"},
        {"grant", "Allow"},
        {"deny", "Don't Allow"},
        {"choices", QJsonArray()},
    };
    askShell(handle, "access.qml", QJsonDocument(question).toJson(QJsonDocument::Compact), {},
             [answer](const std::optional<QByteArray>& out) -> QVariantList {
                 const QJsonObject reply = out ? QJsonDocument::fromJson(*out).object() : QJsonObject();
                 return answer(reply.value("response").toInt(2) == 0);
             },
             false, parentWindow);
}

QString appName(const QString& app) {
    return app.isEmpty() ? QStringLiteral("An app") : app;
}

// Opens the compositor's side of a session over the session's own link.
bool open(InputState* st, const char* cmd, uint devices) {
    st->link = std::make_unique<IpcLink>(Compositor::instance()->socket());
    const auto r = st->link->request({{"cmd", cmd}, {"devices", int(devices)}});
    if (!r)
        return false;
    st->cookie = uint(r->toObject().value("cookie").toInt());
    st->granted = devices;
    return st->cookie != 0;
}

} // namespace

// --- InputState -------------------------------------------------------------------

InputState::InputState(PortalSession* session) : QObject(session) {
    setObjectName("input");
}

InputState* InputState::of(PortalSession* session) {
    if (auto* s = session->findChild<InputState*>("input", Qt::FindDirectChildrenOnly))
        return s;
    return new InputState(session);
}

// --- RemoteDesktop ------------------------------------------------------------------

RemoteDesktopAdaptor::RemoteDesktopAdaptor(PortalBackend* backend) : QDBusAbstractAdaptor(backend), backend_(backend) {}

uint RemoteDesktopAdaptor::CreateSession(const QDBusObjectPath&, const QDBusObjectPath& session, const QString& app,
                                         const QVariantMap&, QVariantMap& results) {
    auto* s = new PortalSession(session.path(), app, backend_);
    backend_->addSession(s);
    InputState::of(s);
    CastState::of(s)->types = 0;  // nothing to share unless ScreenCast.SelectSources says so
    results = {{"session_id", session.path()}};
    return 0;
}

uint RemoteDesktopAdaptor::SelectDevices(const QDBusObjectPath&, const QDBusObjectPath& session, const QString&,
                                         const QVariantMap& options, QVariantMap&) {
    PortalSession* s = backend_->session(session.path());
    if (!s)
        return 2;
    InputState* st = InputState::of(s);
    st->devices = options.value("types", 7u).toUInt() & deviceTypes();
    st->persist = std::min(options.value("persist_mode", 0u).toUInt(), 2u);
    if (!st->devices)
        return 2;
    // Remembered: the devices, and the screen with them.
    if (const auto kept = options.contains("restore_data")
            ? ScreenCastAdaptor::restored(options.value("restore_data")) : std::nullopt) {
        const uint devices = kept->value("devices").toUInt() & st->devices;
        if (devices) {
            st->devices = devices;
            st->restored = true;
            CastState::of(s)->restored = ScreenCastAdaptor::decode(*kept);
        }
    }
    return 0;
}

uint RemoteDesktopAdaptor::Start(const QDBusObjectPath& handle, const QDBusObjectPath& session, const QString& app,
                                 const QString& parentWindow, const QVariantMap&, QVariantMap& results) {
    PortalSession* s = backend_->session(session.path());
    if (!s)
        return 2;
    InputState* st = InputState::of(s);
    CastState* cast = CastState::of(s);
    const bool share = cast->selected && cast->types;

    // What Start answers once the user agreed (or had before): the devices,
    // the stream, and what to remember.
    auto finish = [st, s](const std::optional<cast::Choice>& choice) -> std::optional<QVariantMap> {
        if (!open(st, "remote.start", st->devices))
            return std::nullopt;
        QVariantMap r;
        QVariantMap keep{{"devices", st->granted}};
        if (choice) {
            const auto c = ScreenCastAdaptor::begin(s, *choice);
            if (!c)
                return std::nullopt;
            r = *c;
            keep.insert(ScreenCastAdaptor::encode(*choice));
        }
        r.insert("devices", st->granted);
        r.insert("clipboard_enabled", false);
        r.insert("persist_mode", st->persist);
        if (st->persist)
            r.insert("restore_data", ScreenCastAdaptor::restoreData(keep));
        else
            r.remove("restore_data");
        return r;
    };

    if (st->restored) {
        std::optional<cast::Choice> choice;
        if (share && cast->restored)
            choice = ScreenCastAdaptor::resolve(*cast->restored);
        if (!share || choice) {
            if (const auto r = finish(choice)) {
                results = *r;
                return 0;
            }
        }
    }

    QPointer<PortalSession> guard(s);
    if (share) {
        const bool cursor = cast->cursor == cast::Embedded;
        askShell(handle.path(), "share.qml", ScreenCastAdaptor::offer(cast->types), "remote",
                 [guard, cursor, finish](const std::optional<QByteArray>& out) -> QVariantList {
                     if (!guard)
                         return {uint(2), QVariantMap()};
                     const auto c = out ? ScreenCastAdaptor::chosen(*out, cursor) : std::nullopt;
                     if (!c)
                         return {uint(1), QVariantMap()};
                     const auto r = finish(c);
                     return {uint(r ? 0 : 2), r.value_or(QVariantMap())};
                 },
                 false, parentWindow);
        return 2;
    }
    askAllow(handle.path(), app, QStringLiteral("Allow remote control?"),
             appName(app) + QStringLiteral(" wants to use your keyboard and pointer."), parentWindow,
             [guard, finish](bool allowed) -> QVariantList {
                 if (!guard)
                     return {uint(2), QVariantMap()};
                 if (!allowed)
                     return {uint(1), QVariantMap()};
                 const auto r = finish(std::nullopt);
                 return {uint(r ? 0 : 2), r.value_or(QVariantMap())};
             });
    return 2;  // unused: the reply goes later
}

void RemoteDesktopAdaptor::input(const QDBusObjectPath& session, QJsonObject event) {
    PortalSession* s = backend_->session(session.path());
    InputState* st = s ? InputState::of(s) : nullptr;
    if (!st || !st->link)
        return;
    st->link->request({{"cmd", "remote.input"}, {"cookie", int(st->cookie)}, {"event", event}});
}

std::optional<QPointF> RemoteDesktopAdaptor::place(PortalSession* s, uint stream, double x, double y) const {
    CastState* cast = CastState::of(s);
    if (!cast->stream || cast->stream->node() != stream)
        return std::nullopt;
    const CastStream::Target& t = cast->stream->target();
    if (t.type == cast::Monitor) {
        WaylandLink* link = WaylandLink::instance();
        if (WaylandLink::Output* o = link ? link->output(t.output) : nullptr)
            return QPointF(o->x + x, o->y + y);
        return std::nullopt;
    }
    // A window: where it is now.
    for (const QVariant& v : Compositor::instance()->windows()) {
        const QVariantMap w = v.toMap();
        if (w.value("identifier").toString() != t.toplevel)
            continue;
        const QVariantMap g = w.value("geometry").toMap();
        return QPointF(g.value("x").toDouble() + x, g.value("y").toDouble() + y);
    }
    return std::nullopt;
}

void RemoteDesktopAdaptor::NotifyPointerMotion(const QDBusObjectPath& session, const QVariantMap&, double dx, double dy) {
    input(session, {{"type", "motion"}, {"dx", dx}, {"dy", dy}});
}

void RemoteDesktopAdaptor::NotifyPointerMotionAbsolute(const QDBusObjectPath& session, const QVariantMap&, uint stream,
                                                       double x, double y) {
    PortalSession* s = backend_->session(session.path());
    if (const auto p = s ? place(s, stream, x, y) : std::nullopt)
        input(session, {{"type", "absolute"}, {"x", p->x()}, {"y", p->y()}});
}

void RemoteDesktopAdaptor::NotifyPointerButton(const QDBusObjectPath& session, const QVariantMap&, int button, uint state) {
    input(session, {{"type", "button"}, {"button", button}, {"pressed", state == 1}});
}

void RemoteDesktopAdaptor::NotifyPointerAxis(const QDBusObjectPath& session, const QVariantMap& options, double dx, double dy) {
    input(session, {{"type", "axis"}, {"dx", dx}, {"dy", dy}, {"finish", options.value("finish").toBool()}});
}

void RemoteDesktopAdaptor::NotifyPointerAxisDiscrete(const QDBusObjectPath& session, const QVariantMap&, uint axis, int steps) {
    input(session, {{"type", "axis_discrete"}, {"axis", int(axis)}, {"steps", steps}});
}

void RemoteDesktopAdaptor::NotifyKeyboardKeycode(const QDBusObjectPath& session, const QVariantMap&, int keycode, uint state) {
    input(session, {{"type", "key"}, {"keycode", keycode}, {"pressed", state == 1}});
}

void RemoteDesktopAdaptor::NotifyKeyboardKeysym(const QDBusObjectPath& session, const QVariantMap&, int keysym, uint state) {
    input(session, {{"type", "keysym"}, {"keysym", keysym}, {"pressed", state == 1}});
}

void RemoteDesktopAdaptor::NotifyTouchDown(const QDBusObjectPath& session, const QVariantMap&, uint stream, uint slot,
                                           double x, double y) {
    PortalSession* s = backend_->session(session.path());
    if (const auto p = s ? place(s, stream, x, y) : std::nullopt)
        input(session, {{"type", "touch_down"}, {"slot", int(slot)}, {"x", p->x()}, {"y", p->y()}});
}

void RemoteDesktopAdaptor::NotifyTouchMotion(const QDBusObjectPath& session, const QVariantMap&, uint stream, uint slot,
                                             double x, double y) {
    PortalSession* s = backend_->session(session.path());
    if (const auto p = s ? place(s, stream, x, y) : std::nullopt)
        input(session, {{"type", "touch_motion"}, {"slot", int(slot)}, {"x", p->x()}, {"y", p->y()}});
}

void RemoteDesktopAdaptor::NotifyTouchUp(const QDBusObjectPath& session, const QVariantMap&, uint slot) {
    input(session, {{"type", "touch_up"}, {"slot", int(slot)}});
}

QDBusUnixFileDescriptor RemoteDesktopAdaptor::ConnectToEIS(const QDBusObjectPath& session, const QString&,
                                                           const QVariantMap&) {
    PortalSession* s = backend_->session(session.path());
    InputState* st = s ? InputState::of(s) : nullptr;
    int fd = -1;
    if (!st || !st->link || !st->link->request({{"cmd", "eis.connect"}, {"cookie", int(st->cookie)}}, &fd) || fd < 0) {
        backend_->fail("org.freedesktop.portal.Error.Failed", "The session isn't started");
        return {};
    }
    QDBusUnixFileDescriptor out(fd);  // dups it
    close(fd);
    return out;
}

// --- InputCapture ---------------------------------------------------------------------

InputCaptureAdaptor::InputCaptureAdaptor(PortalBackend* backend) : QDBusAbstractAdaptor(backend), backend_(backend) {
    qDBusRegisterMetaType<CaptureZone>();
    qDBusRegisterMetaType<CaptureZones>();
    qDBusRegisterMetaType<QList<QVariantMap>>();
    connect(Compositor::instance(), &Compositor::captureEvent, this, &InputCaptureAdaptor::compositorEvent);
    // The screens changed: the zones did too.
    connect(Compositor::instance(), &Compositor::outputsChanged, this, [this] {
        for (QObject* o : backend_->children())
            if (auto* s = qobject_cast<PortalSession*>(o))
                if (InputState* st = s->findChild<InputState*>("input"); st && st->capture) {
                    st->zone_set++;
                    emit ZonesChanged(QDBusObjectPath(s->path()), {{"zone_set", st->zone_set}});
                }
    });
}

QVariantMap InputCaptureAdaptor::CreateSession2(const QDBusObjectPath& session, const QString& app, const QVariantMap&) {
    auto* s = new PortalSession(session.path(), app, backend_);
    backend_->addSession(s);
    InputState::of(s)->capture = true;
    return {};
}

InputState* InputCaptureAdaptor::started(const QDBusObjectPath& session) const {
    PortalSession* s = backend_->session(session.path());
    InputState* st = s ? s->findChild<InputState*>("input") : nullptr;
    return st && st->capture && st->link ? st : nullptr;
}

uint InputCaptureAdaptor::Start(const QDBusObjectPath& handle, const QDBusObjectPath& session, const QString& app,
                                const QString& parentWindow, const QVariantMap& options, QVariantMap& results) {
    PortalSession* s = backend_->session(session.path());
    if (!s)
        return 2;
    InputState* st = InputState::of(s);
    st->capture = true;
    st->devices = options.value("capabilities").toUInt() & capabilities();
    st->persist = std::min(options.value("persist_mode", 0u).toUInt(), 2u);
    if (!st->devices)
        return 2;
    auto finish = [st]() -> std::optional<QVariantMap> {
        if (!open(st, "capture.create", st->devices))
            return std::nullopt;
        QVariantMap r{{"capabilities", st->granted}, {"clipboard_enabled", false}};
        if (st->persist)
            r.insert("restore_data", ScreenCastAdaptor::restoreData({{"capabilities", st->granted}}));
        return r;
    };
    if (const auto kept = options.contains("restore_data")
            ? ScreenCastAdaptor::restored(options.value("restore_data")) : std::nullopt) {
        const uint caps = kept->value("capabilities").toUInt() & st->devices;
        if (caps == st->devices)
            if (const auto r = finish()) {
                results = *r;
                return 0;
            }
    }
    QPointer<PortalSession> guard(s);
    askAllow(handle.path(), app, QStringLiteral("Share your keyboard and mouse?"),
             appName(app) + QStringLiteral(" wants to take your keyboard and mouse when the pointer leaves the "
                                           "screen, to use them on another computer. Super+Shift+Escape takes them back."),
             parentWindow, [guard, finish](bool allowed) -> QVariantList {
                 if (!guard)
                     return {uint(2), QVariantMap()};
                 if (!allowed)
                     return {uint(1), QVariantMap()};
                 const auto r = finish();
                 return {uint(r ? 0 : 2), r.value_or(QVariantMap())};
             });
    return 2;
}

uint InputCaptureAdaptor::GetZones(const QDBusObjectPath&, const QDBusObjectPath& session, const QString&,
                                   const QVariantMap&, QVariantMap& results) {
    InputState* st = started(session);
    WaylandLink* link = WaylandLink::instance();
    if (!st || !link)
        return 2;
    link->roundtrip();
    CaptureZones zones;
    for (const auto& o : link->outputs())
        if (o->width > 0)
            zones.append({uint(o->width), uint(o->height), o->x, o->y});
    results = {{"zones", QVariant::fromValue(zones)}, {"zone_set", st->zone_set}};
    return 0;
}

uint InputCaptureAdaptor::SetPointerBarriers(const QDBusObjectPath&, const QDBusObjectPath& session, const QString&,
                                             const QVariantMap&, const QList<QVariantMap>& barriers, uint zoneSet,
                                             QVariantMap& results) {
    InputState* st = started(session);
    if (!st)
        return 2;
    QJsonArray list;
    QList<uint> failed;
    for (const QVariantMap& b : barriers) {
        const uint id = b.value("barrier_id").toUInt();
        int x1 = 0, y1 = 0, x2 = 0, y2 = 0;
        const QVariant pos = b.value("position");
        if (pos.canConvert<QDBusArgument>()) {
            const QDBusArgument arg = pos.value<QDBusArgument>();
            arg.beginStructure();
            arg >> x1 >> y1 >> x2 >> y2;
            arg.endStructure();
        }
        // Barriers set against zones that have since changed are refused.
        if (!id || zoneSet != st->zone_set) {
            failed.append(id);
            continue;
        }
        list.append(QJsonObject{{"id", int(id)}, {"x1", x1}, {"y1", y1}, {"x2", x2}, {"y2", y2}});
    }
    const auto r = st->link->request({{"cmd", "capture.barriers"}, {"cookie", int(st->cookie)}, {"barriers", list}});
    if (!r)
        return 2;
    for (const QJsonValue& v : r->toObject().value("failed").toArray())
        failed.append(uint(v.toInt()));
    results = {{"failed_barriers", QVariant::fromValue(failed)}};
    return 0;
}

uint InputCaptureAdaptor::Enable(const QDBusObjectPath& session, const QString&, const QVariantMap&, QVariantMap&) {
    InputState* st = started(session);
    return st && st->link->request({{"cmd", "capture.enable"}, {"cookie", int(st->cookie)}}) ? 0 : 2;
}

uint InputCaptureAdaptor::Disable(const QDBusObjectPath& session, const QString&, const QVariantMap&, QVariantMap&) {
    InputState* st = started(session);
    if (!st || !st->link->request({{"cmd", "capture.disable"}, {"cookie", int(st->cookie)}}))
        return 2;
    emit Disabled(session, {});
    return 0;
}

uint InputCaptureAdaptor::Release(const QDBusObjectPath& session, const QString&, const QVariantMap& options, QVariantMap&) {
    InputState* st = started(session);
    if (!st)
        return 2;
    QJsonObject req{{"cmd", "capture.release"}, {"cookie", int(st->cookie)}};
    if (options.contains("cursor_position")) {
        const QPointF p = qdbus_cast<QPointF>(options.value("cursor_position"));
        req["x"] = p.x();
        req["y"] = p.y();
    }
    st->link->request(req);
    return 0;
}

QDBusUnixFileDescriptor InputCaptureAdaptor::ConnectToEIS(const QDBusObjectPath& session, const QString&,
                                                          const QVariantMap&) {
    InputState* st = started(session);
    int fd = -1;
    if (!st || !st->link->request({{"cmd", "eis.connect"}, {"cookie", int(st->cookie)}}, &fd) || fd < 0) {
        backend_->fail("org.freedesktop.portal.Error.Failed", "The session isn't started");
        return {};
    }
    QDBusUnixFileDescriptor out(fd);
    close(fd);
    return out;
}

void InputCaptureAdaptor::compositorEvent(const QJsonObject& e) {
    const uint cookie = uint(e.value("cookie").toInt());
    for (QObject* o : backend_->children()) {
        auto* s = qobject_cast<PortalSession*>(o);
        InputState* st = s ? s->findChild<InputState*>("input") : nullptr;
        if (!st || !st->capture || st->cookie != cookie)
            continue;
        const QDBusObjectPath path(s->path());
        const QString kind = e.value("event").toString();
        const QPointF at(e.value("x").toDouble(), e.value("y").toDouble());
        if (kind == "capture.activated")
            emit Activated(path, {{"activation_id", uint(e.value("activation_id").toInt())},
                                  {"cursor_position", at}, {"barrier_id", uint(e.value("barrier").toInt())}});
        else if (kind == "capture.deactivated")
            emit Deactivated(path, {{"activation_id", uint(e.value("activation_id").toInt())}, {"cursor_position", at}});
        else if (kind == "capture.disabled")
            emit Disabled(path, {});
        return;
    }
}

} // namespace atrium
