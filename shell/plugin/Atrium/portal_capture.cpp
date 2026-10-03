#include "portal_capture.hpp"

#include "compositor.hpp"
#include "portal_extras.hpp"

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMetaType>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstring>

namespace atrium {

QDBusArgument& operator<<(QDBusArgument& arg, const PortalZone& z) {
    arg.beginStructure();
    arg << z.width << z.height << z.x << z.y;
    arg.endStructure();
    return arg;
}

const QDBusArgument& operator>>(const QDBusArgument& arg, PortalZone& z) {
    arg.beginStructure();
    arg >> z.width >> z.height >> z.x >> z.y;
    arg.endStructure();
    return arg;
}

namespace {

const QString kPath = QStringLiteral("/org/freedesktop/portal/desktop");

QDBusArgument point(double x, double y) {
    QDBusArgument a;
    a.beginStructure();
    a << x << y;
    a.endStructure();
    return a;
}

int connectTo(const QString& path) {
    const QByteArray p = path.toLocal8Bit();
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (size_t(p.size()) >= sizeof addr.sun_path)
        return -1;
    std::memcpy(addr.sun_path, p.constData(), size_t(p.size()));
    const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd >= 0 && ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

} // namespace

InputCaptureAdaptor::InputCaptureAdaptor(PortalBackend* parent) : QDBusAbstractAdaptor(parent) {
    qDBusRegisterMetaType<PortalZone>();
    qDBusRegisterMetaType<PortalZones>();
    qDBusRegisterMetaType<QList<QVariantMap>>();
    Compositor* c = Compositor::instance();
    // Screens changed: new zones, and barriers set for the old ones stop.
    connect(c, &Compositor::outputsChanged, this, [this] {
        ++zoneSet_;
        for (const QString& session : captures_.keys())
            signal(session, "ZonesChanged", {{"zone_set", zoneSet_}});
    });
    connect(c, &Compositor::captureEvent, this, [this](const QString& kind, const QVariantMap& e) {
        const QString session = sessionOf(e.value("session").toLongLong());
        if (session.isEmpty())
            return;
        const uint activation = e.value("activation").toUInt();
        if (kind == "activated")
            signal(session, "Activated",
                   {{"activation_id", activation},
                    {"cursor_position", QVariant::fromValue(point(e.value("x").toDouble(), e.value("y").toDouble()))},
                    {"barrier_id", e.value("barrier").toUInt()}});
        else if (kind == "deactivated")
            signal(session, "Deactivated",
                   {{"activation_id", activation},
                    {"cursor_position", QVariant::fromValue(point(e.value("x").toDouble(), e.value("y").toDouble()))}});
        else if (kind == "disabled")
            signal(session, "Disabled", {});
    });
    // atrium going ends every session.
    connect(c, &Compositor::connectedChanged, this, [this] {
        if (Compositor::instance()->connected())
            return;
        for (const QString& path : captures_.keys())
            if (PortalSession* s = PortalBackend::instance()->session(path))
                s->end();
    });
}

QString InputCaptureAdaptor::sessionOf(qint64 id) const {
    for (auto it = captures_.begin(); it != captures_.end(); ++it)
        if (it->id == id)
            return it.key();
    return {};
}

void InputCaptureAdaptor::signal(const QString& session, const QString& name, const QVariantMap& options) {
    QDBusMessage m = QDBusMessage::createSignal(kPath, "org.freedesktop.impl.portal.InputCapture", name);
    m << QVariant::fromValue(QDBusObjectPath(session)) << options;
    QDBusConnection::sessionBus().send(m);
}

void InputCaptureAdaptor::end(const QString& session) {
    const Capture c = captures_.take(session);
    if (c.id)
        Compositor::instance()->eisClose(c.id);
}

uint InputCaptureAdaptor::CreateSession(const QDBusObjectPath& handle, const QDBusObjectPath& session,
                                        const QString& app, const QString&, const QVariantMap& options,
                                        QVariantMap&) {
    const uint wanted = options.value("capabilities").toUInt() & capabilities();
    if (!wanted)
        return 2;
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
    const QString path = session.path();
    auto start = [this, path, app, wanted, answer] {
        Compositor::instance()->captureOpen(wanted, [this, path, app, wanted, answer](const QJsonObject& reply) {
            if (!reply.value("ok").toBool())
                return answer(2, {});
            const QJsonObject r = reply.value("result").toObject();
            auto* s = new PortalSession(path, app, PortalBackend::instance());
            PortalBackend::instance()->addSession(s);
            captures_.insert(path, Capture{r.value("session").toInteger(), r.value("path").toString()});
            connect(s, &QObject::destroyed, this, [this, path] { end(path); });
            answer(0, {{"capabilities", wanted}});
        });
    };

    // Asked first, as for any access.
    const QString name = appDisplayName(app);
    const QJsonObject question{
        {"app", app},
        {"title", app.isEmpty() ? QString("Allow an app to take the keyboard and mouse?")
                                : QString("Allow “%1” to take the keyboard and mouse?").arg(name)},
        {"subtitle", QString()},
        {"body", QString("When the pointer goes off the edge of the screen, it works another computer "
                         "until it comes back. Super+Escape brings it back at any time.")},
        {"grant", "Allow"},
        {"deny", "Don't Allow"},
    };
    auto* dialog = new QProcess(request);
    QObject::connect(request, &PortalRequest::closed, dialog, [dialog] { dialog->kill(); });
    QObject::connect(dialog, &QProcess::finished, request, [dialog, start, answer](int code, QProcess::ExitStatus st) {
        const QJsonObject reply = QJsonDocument::fromJson(dialog->readAllStandardOutput()).object();
        if (st != QProcess::NormalExit || code != 0 || reply.value("response").toInt(2) != 0)
            return answer(1, {});
        start();
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

uint InputCaptureAdaptor::GetZones(const QDBusObjectPath&, const QDBusObjectPath& session, const QString&,
                                   const QVariantMap&, QVariantMap& results) {
    if (!captures_.contains(session.path()))
        return 2;
    PortalZones zones;
    for (const QVariant& o : Compositor::instance()->outputs()) {
        const QVariantMap g = o.toMap().value("geometry").toMap();
        if (g.value("width").toInt() > 0)
            zones.append({uint(g.value("width").toInt()), uint(g.value("height").toInt()), g.value("x").toInt(),
                          g.value("y").toInt()});
    }
    results.insert("zones", QVariant::fromValue(zones));
    results.insert("zone_set", zoneSet_);
    return 0;
}

uint InputCaptureAdaptor::SetPointerBarriers(const QDBusObjectPath&, const QDBusObjectPath& session,
                                             const QString&, const QVariantMap&, const QList<QVariantMap>& barriers,
                                             uint zoneSet, QVariantMap& results) {
    auto it = captures_.find(session.path());
    if (it == captures_.end())
        return 2;
    QList<uint> all;
    QJsonArray list;
    for (const QVariantMap& b : barriers) {
        const uint id = b.value("barrier_id").toUInt();
        all.append(id);
        int x1 = 0, y1 = 0, x2 = 0, y2 = 0;
        if (b.value("position").canConvert<QDBusArgument>()) {
            const QDBusArgument p = b.value("position").value<QDBusArgument>();
            p.beginStructure();
            p >> x1 >> y1 >> x2 >> y2;
            p.endStructure();
        }
        list.append(QJsonObject{{"id", qint64(id)}, {"x1", x1}, {"y1", y1}, {"x2", x2}, {"y2", y2}});
    }
    // Barriers for screens that have since changed: none of them.
    if (zoneSet != zoneSet_) {
        results.insert("failed_barriers", QVariant::fromValue(all));
        return 0;
    }
    PortalBackend* backend = PortalBackend::instance();
    const QDBusMessage call = backend->delayReply();
    Compositor::instance()->captureCall("barriers", {{"session", it->id}, {"barriers", list}},
                                        [call, all](const QJsonObject& reply) {
        QList<uint> failed;
        if (!reply.value("ok").toBool())
            failed = all;
        for (const QJsonValue& v : reply.value("result").toObject().value("failed").toArray())
            failed.append(uint(v.toInteger()));
        QDBusConnection::sessionBus().send(
            call.createReply(QVariantList{uint(0), QVariantMap{{"failed_barriers", QVariant::fromValue(failed)}}}));
    });
    return 0;  // unused: the reply goes later
}

uint InputCaptureAdaptor::Enable(const QDBusObjectPath& session, const QString&, const QVariantMap&, QVariantMap&) {
    auto it = captures_.find(session.path());
    if (it == captures_.end())
        return 2;
    Compositor::instance()->captureCall("enable", {{"session", it->id}}, [](const QJsonObject&) {});
    return 0;
}

uint InputCaptureAdaptor::Disable(const QDBusObjectPath& session, const QString&, const QVariantMap&,
                                  QVariantMap&) {
    auto it = captures_.find(session.path());
    if (it == captures_.end())
        return 2;
    Compositor::instance()->captureCall("disable", {{"session", it->id}}, [](const QJsonObject&) {});
    return 0;
}

uint InputCaptureAdaptor::Release(const QDBusObjectPath& session, const QString&, const QVariantMap& options,
                                  QVariantMap&) {
    auto it = captures_.find(session.path());
    if (it == captures_.end())
        return 2;
    QJsonObject fields{{"session", it->id}};
    if (options.value("cursor_position").canConvert<QDBusArgument>()) {
        double x = 0, y = 0;
        const QDBusArgument p = options.value("cursor_position").value<QDBusArgument>();
        p.beginStructure();
        p >> x >> y;
        p.endStructure();
        fields["x"] = x;
        fields["y"] = y;
    }
    Compositor::instance()->captureCall("release", fields, [](const QJsonObject&) {});
    return 0;
}

QDBusUnixFileDescriptor InputCaptureAdaptor::ConnectToEIS(const QDBusObjectPath& session, const QString&,
                                                          const QVariantMap&) {
    auto it = captures_.find(session.path());
    if (it == captures_.end())
        return {};
    const int fd = connectTo(it->path);
    if (fd < 0)
        return {};
    QDBusUnixFileDescriptor out(fd);  // a dup
    ::close(fd);
    return out;
}

} // namespace atrium
