#include "screencast.hpp"

#include "portal.hpp"
#include "share_core.hpp"
#include "wayland_link.hpp"

#include <QDBusMetaType>
#include <QPoint>
#include <QSize>

namespace atrium {

QDBusArgument& operator<<(QDBusArgument& arg, const RestoreData& r) {
    arg.beginStructure();
    arg << r.vendor << r.version << r.data;
    arg.endStructure();
    return arg;
}

const QDBusArgument& operator>>(const QDBusArgument& arg, RestoreData& r) {
    arg.beginStructure();
    arg >> r.vendor >> r.version >> r.data;
    arg.endStructure();
    return arg;
}

QDBusArgument& operator<<(QDBusArgument& arg, const CastStreamInfo& s) {
    arg.beginStructure();
    arg << s.node << s.props;
    arg.endStructure();
    return arg;
}

const QDBusArgument& operator>>(const QDBusArgument& arg, CastStreamInfo& s) {
    arg.beginStructure();
    arg >> s.node >> s.props;
    arg.endStructure();
    return arg;
}

QVariantMap ScreenCastAdaptor::encode(const cast::Choice& c) {
    QVariantMap m{{"type", c.type}, {"cursor", c.cursor}};
    if (c.type == cast::Monitor) {
        m.insert("output", QString::fromStdString(c.output));
    } else {
        m.insert("app_id", QString::fromStdString(c.app_id));
        m.insert("title", QString::fromStdString(c.title));
    }
    return m;
}

std::optional<QVariantMap> ScreenCastAdaptor::restored(const QVariant& v) {
    if (!v.canConvert<QDBusArgument>())
        return std::nullopt;
    RestoreData r;
    v.value<QDBusArgument>() >> r;
    if (r.vendor != cast::kRestoreVendor || r.version != cast::kRestoreVersion)
        return std::nullopt;  // another portal's, or an older form
    const QVariant inner = r.data.variant();
    return inner.canConvert<QDBusArgument>() ? qdbus_cast<QVariantMap>(inner) : inner.toMap();
}

QVariant ScreenCastAdaptor::restoreData(const QVariantMap& m) {
    return QVariant::fromValue(RestoreData{cast::kRestoreVendor, cast::kRestoreVersion, QDBusVariant(m)});
}

std::optional<cast::Choice> ScreenCastAdaptor::decode(const QVariantMap& m) {
    if (!m.contains("type"))
        return std::nullopt;
    cast::Choice c;
    c.type = m.value("type").toUInt();
    c.cursor = m.value("cursor", true).toBool();
    c.output = m.value("output").toString().toStdString();
    c.app_id = m.value("app_id").toString().toStdString();
    c.title = m.value("title").toString().toStdString();
    if (c.type != cast::Monitor && c.type != cast::Window)
        return std::nullopt;
    return c;
}

// --- CastState -----------------------------------------------------------------

CastState::CastState(PortalSession* session) : QObject(session) {
    setObjectName("cast");
}

CastState* CastState::of(PortalSession* session) {
    if (auto* s = session->findChild<CastState*>("cast", Qt::FindDirectChildrenOnly))
        return s;
    return new CastState(session);
}

// --- ScreenCast -------------------------------------------------------------------

ScreenCastAdaptor::ScreenCastAdaptor(PortalBackend* backend) : QDBusAbstractAdaptor(backend), backend_(backend) {
    qDBusRegisterMetaType<RestoreData>();
    qDBusRegisterMetaType<CastStreamInfo>();
    qDBusRegisterMetaType<CastStreamInfos>();
}

uint ScreenCastAdaptor::CreateSession(const QDBusObjectPath&, const QDBusObjectPath& session, const QString& app,
                                      const QVariantMap&, QVariantMap& results) {
    auto* s = new PortalSession(session.path(), app, backend_);
    backend_->addSession(s);
    CastState::of(s);
    results = {{"session_id", session.path()}};
    return 0;
}

uint ScreenCastAdaptor::SelectSources(const QDBusObjectPath&, const QDBusObjectPath& session, const QString&,
                                      const QVariantMap& options, QVariantMap&) {
    PortalSession* s = backend_->session(session.path());
    if (!s)
        return 2;
    CastState* st = CastState::of(s);
    if (options.contains("types"))
        st->types = options.value("types").toUInt() & sourceTypes();
    if (!st->types)
        return 2;
    // Metadata (the pointer beside the picture) isn't offered: it's drawn in.
    const uint cursor = options.value("cursor_mode", uint(cast::Embedded)).toUInt();
    st->cursor = cursor == cast::Hidden ? cast::Hidden : cast::Embedded;
    st->persist = std::min(options.value("persist_mode", 0u).toUInt(), uint(cast::PersistPermanent));
    const auto kept = options.contains("restore_data") ? restored(options.value("restore_data")) : std::nullopt;
    st->restored = kept ? decode(*kept) : std::nullopt;
    st->selected = true;
    return 0;
}

QByteArray ScreenCastAdaptor::offer(uint types) {
    QByteArray out;
    WaylandLink* link = WaylandLink::instance();
    if (!link)
        return out;
    link->roundtrip();  // windows opened since
    if (types & cast::Monitor)
        for (const auto& o : link->outputs())
            out += ("Monitor: " + o->name + ' ' + o->description + '\n').toUtf8();
    if (types & cast::Window)
        for (const auto& t : link->toplevels())
            out += ("Window: " + QString(t->title).replace('\n', ' ') + " (" + t->identifier + ")\n").toUtf8();
    return out;
}

std::optional<cast::Choice> ScreenCastAdaptor::chosen(const QByteArray& out, bool cursor) {
    const std::vector<share::Source> picked = share::parse(out.toStdString());
    if (picked.empty())
        return std::nullopt;
    const share::Source& p = picked.front();
    cast::Choice c;
    c.cursor = cursor;
    if (p.kind == share::Source::Kind::Screen) {
        c.type = cast::Monitor;
        c.output = p.name;
        return c;
    }
    WaylandLink* link = WaylandLink::instance();
    WaylandLink::Toplevel* t = link ? link->toplevel(QString::fromStdString(p.name)) : nullptr;
    if (!t) {
        qWarning("screencast: the chosen window %s is gone", p.name.c_str());
        return std::nullopt;
    }
    c.type = cast::Window;
    c.app_id = t->app_id.toStdString();
    c.title = t->title.toStdString();
    c.window = p.name;
    return c;
}

std::optional<cast::Choice> ScreenCastAdaptor::resolve(const cast::Choice& c) {
    WaylandLink* link = WaylandLink::instance();
    if (!link)
        return std::nullopt;
    link->roundtrip();
    if (c.type == cast::Monitor)
        return link->output(QString::fromStdString(c.output)) ? std::optional(c) : std::nullopt;
    std::vector<cast::Candidate> windows;
    for (const auto& t : link->toplevels())
        windows.push_back({t->identifier.toStdString(), t->app_id.toStdString(), t->title.toStdString()});
    const auto id = cast::match_window(c, windows);
    if (!id)
        return std::nullopt;
    cast::Choice out = c;
    out.window = *id;
    return out;
}

std::optional<QVariantMap> ScreenCastAdaptor::begin(PortalSession* session, const cast::Choice& choice) {
    CastState* st = CastState::of(session);
    delete st->stream;
    CastStream::Target target;
    target.type = choice.type;
    target.cursor = choice.cursor;
    if (choice.type == cast::Monitor)
        target.output = QString::fromStdString(choice.output);
    else
        target.toplevel = QString::fromStdString(choice.window);
    auto* stream = new CastStream(target, st);
    if (!stream->start() || !stream->wait_ready(3000)) {
        qWarning("screencast: the stream didn't start");
        delete stream;
        return std::nullopt;
    }
    st->stream = stream;
    st->chosen = choice;
    // The window closed or the screen went: the session ends, and the app
    // hears so.
    QObject::connect(stream, &CastStream::stopped, session, [session] {
        emit session->Closed();
        session->Close();
    });

    QVariantMap props{{"source_type", choice.type}, {"size", QSize(stream->width(), stream->height())}};
    if (choice.type == cast::Monitor) {
        if (WaylandLink::Output* o = WaylandLink::instance()->output(target.output)) {
            props.insert("position", QPoint(o->x, o->y));
            props.insert("size", QSize(o->width, o->height));  // logical, for pointer positions
        }
        props.insert("mapping_id", target.output);
    }
    if (stream->serial())
        props.insert("pipewire-serial", qulonglong(stream->serial()));
    QVariantMap results{
        {"streams", QVariant::fromValue(CastStreamInfos{{stream->node(), props}})},
        {"persist_mode", st->persist},
    };
    if (st->persist != cast::PersistNone) {
        results.insert("restore_data", restoreData(encode(choice)));
    }
    return results;
}

uint ScreenCastAdaptor::Start(const QDBusObjectPath& handle, const QDBusObjectPath& session, const QString&,
                              const QString& parentWindow, const QVariantMap&, QVariantMap& results) {
    PortalSession* s = backend_->session(session.path());
    if (!s)
        return 2;
    CastState* st = CastState::of(s);
    // Remembered, and still there: no need to ask.
    if (st->restored) {
        if (auto c = resolve(*st->restored)) {
            if (auto r = begin(s, *c)) {
                results = *r;
                return 0;
            }
        }
    }
    const bool cursor = st->cursor == cast::Embedded;
    QPointer<PortalSession> guard(s);
    askShell(handle.path(), "share.qml", offer(st->types), {},
             [guard, cursor](const std::optional<QByteArray>& out) -> QVariantList {
                 if (!guard)
                     return {uint(2), QVariantMap()};
                 const auto c = out ? chosen(*out, cursor) : std::nullopt;
                 if (!c)
                     return {uint(1), QVariantMap()};
                 const auto r = begin(guard, *c);
                 return {uint(r ? 0 : 2), r.value_or(QVariantMap())};
             },
             false, parentWindow);
    return 2;  // unused: the reply goes later
}

} // namespace atrium
