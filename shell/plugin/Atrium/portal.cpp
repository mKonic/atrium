#include "portal.hpp"

#include "compositor.hpp"
#include "paths.hpp"
#include "portal_extras.hpp"
#include "portal_files.hpp"
#include "share_core.hpp"

#include <QColor>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>

#include <algorithm>
#include <functional>
#include <optional>

namespace atrium {

namespace {

const QString kName = QStringLiteral("org.freedesktop.impl.portal.desktop.atrium");
const QString kPath = QStringLiteral("/org/freedesktop/portal/desktop");

QDBusConnection bus() {
    return QDBusConnection::sessionBus();
}

// Apps the portal can't name (not sandboxed, not registered) share one.
QString appKey(const QString& app) {
    return app.isEmpty() ? QStringLiteral("app") : app;
}

} // namespace

QDBusArgument& operator<<(QDBusArgument& arg, const PortalStream& s) {
    arg.beginStructure();
    arg << s.node << s.properties;
    arg.endStructure();
    return arg;
}

const QDBusArgument& operator>>(const QDBusArgument& arg, PortalStream& s) {
    arg.beginStructure();
    arg >> s.node >> s.properties;
    arg.endStructure();
    return arg;
}

QDBusArgument& operator<<(QDBusArgument& arg, const PortalRestore& r) {
    arg.beginStructure();
    arg << r.vendor << r.version << r.data;
    arg.endStructure();
    return arg;
}

const QDBusArgument& operator>>(const QDBusArgument& arg, PortalRestore& r) {
    arg.beginStructure();
    arg >> r.vendor >> r.version >> r.data;
    arg.endStructure();
    return arg;
}

QDBusArgument& operator<<(QDBusArgument& arg, const PortalColor& c) {
    arg.beginStructure();
    arg << c.r << c.g << c.b;
    arg.endStructure();
    return arg;
}

const QDBusArgument& operator>>(const QDBusArgument& arg, PortalColor& c) {
    arg.beginStructure();
    arg >> c.r >> c.g >> c.b;
    arg.endStructure();
    return arg;
}

QDBusArgument& operator<<(QDBusArgument& arg, const PortalPair& p) {
    arg.beginStructure();
    arg << p.id << p.label;
    arg.endStructure();
    return arg;
}

const QDBusArgument& operator>>(const QDBusArgument& arg, PortalPair& p) {
    arg.beginStructure();
    arg >> p.id >> p.label;
    arg.endStructure();
    return arg;
}

QDBusArgument& operator<<(QDBusArgument& arg, const PortalChoice& c) {
    arg.beginStructure();
    arg << c.id << c.label << c.options << c.initial;
    arg.endStructure();
    return arg;
}

const QDBusArgument& operator>>(const QDBusArgument& arg, PortalChoice& c) {
    arg.beginStructure();
    arg >> c.id >> c.label >> c.options >> c.initial;
    arg.endStructure();
    return arg;
}

QDBusArgument& operator<<(QDBusArgument& arg, const PortalShortcut& s) {
    arg.beginStructure();
    arg << s.id << s.options;
    arg.endStructure();
    return arg;
}

const QDBusArgument& operator>>(const QDBusArgument& arg, PortalShortcut& s) {
    arg.beginStructure();
    arg >> s.id >> s.options;
    arg.endStructure();
    return arg;
}

// --- PortalSession -----------------------------------------------------------

PortalSession::PortalSession(const QString& path, const QString& app, QObject* parent)
    : QObject(parent), path_(path), app_(appKey(app)) {
    bus().registerObject(path, this, QDBusConnection::ExportAllSlots | QDBusConnection::ExportAllSignals |
                                         QDBusConnection::ExportAllProperties);
}

PortalSession::~PortalSession() {
    bus().unregisterObject(path_);
}

void PortalSession::Close() {
    PortalBackend::instance()->dropSession(path_);
    deleteLater();
}

void PortalSession::end() {
    // Sent by hand: QtDBus doesn't relay this object's signals.
    bus().send(QDBusMessage::createSignal(path_, QStringLiteral("org.freedesktop.impl.portal.Session"),
                                          QStringLiteral("Closed")));
    Close();
}

// --- PortalBackend -----------------------------------------------------------

PortalBackend* PortalBackend::instance() {
    static auto* self = new PortalBackend;
    return self;
}

PortalBackend::PortalBackend() {
    qDBusRegisterMetaType<PortalShortcut>();
    qDBusRegisterMetaType<PortalShortcuts>();
    qDBusRegisterMetaType<PortalColor>();
    qDBusRegisterMetaType<PortalNamespaces>();
    qDBusRegisterMetaType<PortalPair>();
    qDBusRegisterMetaType<PortalPairs>();
    qDBusRegisterMetaType<PortalChoice>();
    qDBusRegisterMetaType<PortalChoices>();
    qDBusRegisterMetaType<PortalStream>();
    qDBusRegisterMetaType<PortalStreams>();
    qDBusRegisterMetaType<PortalRestore>();
    new GlobalShortcutsAdaptor(this);
    new SettingsAdaptor(this);
    new WallpaperAdaptor(this);
    new AccessAdaptor(this);
    new ScreenshotAdaptor(this);
    new InhibitAdaptor(this);
    new ScreenCastAdaptor(this);
    new EmailAdaptor(this);
    new AccountAdaptor(this);
    new DynamicLauncherAdaptor(this);
    new NotificationAdaptor(this);
    new FileChooserAdaptor(this);
    new AppChooserAdaptor(this);
    connect(Compositor::instance(), &Compositor::portalShortcut, this, &PortalBackend::pressed);
}

bool PortalBackend::start() {
    if (!bus().registerObject(kPath, this, QDBusConnection::ExportAdaptors))
        return false;
    return bus().registerService(kName);
}

void PortalBackend::addSession(PortalSession* s) {
    sessions_.insert(s->path(), s);
}

QHash<QString, QVariantMap> PortalBackend::shortcutsOf(const QString& app) const {
    QHash<QString, QVariantMap> out;
    const QString prefix = appKey(app) + "/";
    for (const QVariant& v : Compositor::instance()->shortcuts()) {
        const QVariantMap s = v.toMap();
        const QString arg = s.value("arg").toString();
        if (s.value("action") == "portal" && arg.startsWith(prefix))
            out.insert(arg.mid(prefix.size()), s);
    }
    return out;
}

QString PortalBackend::describe(const QString& keys) {
    QStringList parts = keys.split('+', Qt::SkipEmptyParts);
    for (QString& p : parts) {
        const QString low = p.toLower();
        p = low == "logo" || low == "super" || low == "mod" ? QStringLiteral("Super")
          : low == "ctrl" || low == "control" ? QStringLiteral("Ctrl")
          : low.size() == 1 ? low.toUpper() : low.left(1).toUpper() + low.mid(1);
    }
    return parts.join('+');
}

// --- GlobalShortcuts ---------------------------------------------------------

GlobalShortcutsAdaptor::GlobalShortcutsAdaptor(PortalBackend* parent) : QDBusAbstractAdaptor(parent), backend_(parent) {
    // Keys changed in Settings: each app hears its new ones.
    connect(Compositor::instance(), &Compositor::shortcutsChanged, this, [this] {
        for (QObject* o : backend_->children())
            if (auto* s = qobject_cast<PortalSession*>(o); s && !s->ids.isEmpty())
                emit ShortcutsChanged(QDBusObjectPath(s->path()), listFor(s));
    });
    connect(parent, &PortalBackend::pressed, this, [this](const QString& app, const QString& id, bool down) {
        const qulonglong now = qulonglong(QDateTime::currentMSecsSinceEpoch());
        // Every session of that app that bound it (an app may hold several).
        for (QObject* o : backend_->children()) {
            auto* s = qobject_cast<PortalSession*>(o);
            if (!s || s->app() != app || !s->ids.contains(id))
                continue;
            if (down)
                emit Activated(QDBusObjectPath(s->path()), id, now, {});
            else
                emit Deactivated(QDBusObjectPath(s->path()), id, now, {});
        }
    });
}

uint GlobalShortcutsAdaptor::CreateSession(const QDBusObjectPath&, const QDBusObjectPath& session, const QString& app,
                                           const QVariantMap&, QVariantMap& results) {
    backend_->addSession(new PortalSession(session.path(), app, backend_));
    results = {{"session_id", session.path()}};
    return 0;
}

PortalShortcuts GlobalShortcutsAdaptor::listFor(PortalSession* s) const {
    PortalShortcuts out;
    const auto have = backend_->shortcutsOf(s->app());
    for (const QString& id : s->ids) {
        const QString keys = have.value(id).value("keys").toString();
        out.append({id, {{"description", s->descriptions.value(id, id)},
                         {"trigger_description", PortalBackend::describe(keys)}}});
    }
    return out;
}

uint GlobalShortcutsAdaptor::BindShortcuts(const QDBusObjectPath&, const QDBusObjectPath& session,
                                           const PortalShortcuts& shortcuts, const QString&, const QVariantMap&,
                                           QVariantMap& results) {
    PortalSession* s = backend_->session(session.path());
    if (!s)
        return 2;
    const auto have = backend_->shortcutsOf(s->app());
    PortalShortcuts out;
    for (const PortalShortcut& sc : shortcuts) {
        QString keys = have.value(sc.id).value("keys").toString();
        // New: the keys the app would like, which Settings can change later.
        if (!have.contains(sc.id)) {
            // The portal's spelling ("CTRL+SHIFT+p", "LOGO+x") in atrium's.
            QStringList parts = sc.options.value("preferred_trigger").toString().split('+', Qt::SkipEmptyParts);
            for (qsizetype i = 0; i + 1 < parts.size(); ++i)
                parts[i] = PortalBackend::describe(parts[i]);
            keys = parts.join('+');
            Compositor::instance()->addShortcut(
                {{"keys", keys}, {"action", "portal"}, {"arg", s->app() + "/" + sc.id}});
        }
        if (!s->ids.contains(sc.id))
            s->ids.append(sc.id);
        s->descriptions.insert(sc.id, sc.options.value("description", sc.id).toString());
        QVariantMap options = sc.options;
        options.remove("preferred_trigger");
        options["trigger_description"] = PortalBackend::describe(keys);
        out.append({sc.id, options});
    }
    results = {{"shortcuts", QVariant::fromValue(out)}};
    return 0;
}

uint GlobalShortcutsAdaptor::ListShortcuts(const QDBusObjectPath&, const QDBusObjectPath& session,
                                           QVariantMap& results) {
    PortalSession* s = backend_->session(session.path());
    if (!s)
        return 2;
    results = {{"shortcuts", QVariant::fromValue(listFor(s))}};
    return 0;
}

void GlobalShortcutsAdaptor::ConfigureShortcuts(const QDBusObjectPath&, const QString&, const QVariantMap&) {
    Compositor::instance()->action("shell", "settings:Keyboard Shortcuts");
}

// --- Settings ----------------------------------------------------------------

namespace {
const QString kAppearance = QStringLiteral("org.freedesktop.appearance");
}

SettingsAdaptor::SettingsAdaptor(PortalBackend* parent) : QDBusAbstractAdaptor(parent) {
    last_ = appearance();
    gnome_settings::watch([this](const QString& ns, const QString& key, const QVariant& value) {
        emit SettingChanged(ns, key, QDBusVariant(value));
    });
    connect(Compositor::instance(), &Compositor::settingsChanged, this, [this] {
        const QVariantMap now = appearance();
        for (auto it = now.begin(); it != now.end(); ++it)
            if (last_.value(it.key()) != it.value())
                emit SettingChanged(kAppearance, it.key(), QDBusVariant(it.value()));
        last_ = now;
    });
}

QVariantMap SettingsAdaptor::appearance() const {
    Compositor* c = Compositor::instance();
    const QVariantMap s = c->settings();
    PortalColor accent;
    const QColor color(c->accentColor(s.value("appearance.accent").toString()));
    if (color.isValid())
        accent = {color.redF(), color.greenF(), color.blueF()};
    return {
        // 1 dark, 2 light.
        {"color-scheme", uint(s.value("appearance.style").toString() == "light" ? 2 : 1)},
        {"accent-color", QVariant::fromValue(accent)},
        {"contrast", uint(s.value("appearance.high_contrast").toBool() ? 1 : 0)},
    };
}

PortalNamespaces SettingsAdaptor::ReadAll(const QStringList& namespaces) {
    PortalNamespaces out;
    // Globs ("org.freedesktop.*") or names; none asks for everything.
    auto wanted = [&](const QString& ns) {
        return namespaces.isEmpty() || std::ranges::any_of(namespaces, [&](const QString& n) {
                   return n == ns || (n.endsWith('*') && ns.startsWith(n.chopped(1)));
               });
    };
    if (wanted(kAppearance))
        out.insert(kAppearance, appearance());
    // GNOME's, for GTK apps (fonts, cursor, title-bar buttons...).
    for (const QString& ns : gnome_settings::namespaces())
        if (wanted(ns))
            out.insert(ns, gnome_settings::read(ns));
    return out;
}

QDBusVariant SettingsAdaptor::Read(const QString& ns, const QString& key) {
    const QVariantMap a = ns == kAppearance                             ? appearance()
                          : gnome_settings::namespaces().contains(ns) ? gnome_settings::read(ns)
                                                                      : QVariantMap();
    if (!a.contains(key)) {
        static_cast<PortalBackend*>(parent())->fail("org.freedesktop.portal.Error.NotFound", "No such setting");
        return {};
    }
    return QDBusVariant(a.value(key));
}

// --- Access ------------------------------------------------------------------

namespace {

// Run from the build tree: its own shell, not an installed (older) one.
bool fromBuildTree() {
    return QCoreApplication::applicationDirPath().startsWith(QStringLiteral(ATRIUM_BUILD_DIR "/"));
}

} // namespace

// An installed file, else the source tree's.
QString shellFile(const QString& name) {
    const QString installed = QStringLiteral(ATRIUM_DATADIR "/shell/") + name;
    return QFileInfo::exists(installed) && !fromBuildTree() ? installed
                                                            : QStringLiteral(ATRIUM_SOURCE_DIR "/shell/") + name;
}

QString shellProgram() {
    const QString installed = QStringLiteral(ATRIUM_BINDIR "/atrium-shell");
    return QFileInfo::exists(installed) && !fromBuildTree() ? installed
                                                            : QStringLiteral(ATRIUM_BUILD_DIR "/shell/host/atrium-shell");
}

PortalRequest::PortalRequest(const QString& path, QObject* parent) : QObject(parent), path_(path) {
    bus().registerObject(path, this, QDBusConnection::ExportAllSlots);
}

PortalRequest::~PortalRequest() {
    bus().unregisterObject(path_);
}

QProcess* askShell(const QString& handle, const QString& file, const QByteArray& input, const QString& mode,
                   Answer answer, bool keepInput) {
    PortalBackend* backend = PortalBackend::instance();
    const QDBusMessage call = backend->delayReply();
    auto* request = new PortalRequest(handle, backend);
    auto* dialog = new QProcess(request);
    auto finish = [call, request, answer](const std::optional<QByteArray>& out) {
        if (request->property("answered").toBool())
            return;
        request->setProperty("answered", true);
        bus().send(call.createReply(answer(out)));
        request->deleteLater();
    };
    QObject::connect(request, &PortalRequest::closed, dialog, [dialog] { dialog->kill(); });
    QObject::connect(dialog, &QProcess::finished, request, [dialog, finish](int code, QProcess::ExitStatus status) {
        if (status == QProcess::NormalExit && code == 0)
            finish(dialog->readAllStandardOutput());
        else
            finish(std::nullopt);
    });
    QObject::connect(dialog, &QProcess::errorOccurred, request, [finish](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart)
            finish(std::nullopt);
    });
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    if (!mode.isEmpty())
        env.insert("ATRIUM_CAPTURE_MODE", mode);
    dialog->setProcessEnvironment(env);
    dialog->start(shellProgram(), {shellFile(file)});
    dialog->write(input);
    if (!keepInput)
        dialog->closeWriteChannel();
    return dialog;
}


uint AccessAdaptor::AccessDialog(const QDBusObjectPath& handle, const QString& app, const QString&,
                                 const QString& title, const QString& subtitle, const QString& body,
                                 const QVariantMap& options, QVariantMap&) {
    PortalChoices choices;
    if (options.contains("choices"))
        options.value("choices").value<QDBusArgument>() >> choices;
    QJsonArray asked;
    for (const PortalChoice& c : choices) {
        QJsonArray opts;
        for (const PortalPair& o : c.options)
            opts.append(QJsonObject{{"id", o.id}, {"label", o.label}});
        asked.append(QJsonObject{{"id", c.id}, {"label", c.label}, {"options", opts}, {"value", c.initial}});
    }
    const QJsonObject question{
        {"app", app},
        {"title", title},
        {"subtitle", subtitle},
        {"body", body},
        {"icon", options.value("icon").toString()},
        {"grant", options.value("grant_label").toString()},
        {"deny", options.value("deny_label").toString()},
        {"choices", asked},
    };
    // 0 allowed, 1 not, 2 closed or failed.
    askShell(handle.path(), "access.qml", QJsonDocument(question).toJson(QJsonDocument::Compact), {},
             [](const std::optional<QByteArray>& out) -> QVariantList {
                 const QJsonObject reply = out ? QJsonDocument::fromJson(*out).object() : QJsonObject();
                 if (!reply.contains("response"))
                     return {uint(2), QVariantMap()};
                 const uint response = uint(reply.value("response").toInt(2));
                 QVariantMap results;
                 if (response == 0) {
                     PortalPairs picked;
                     const QJsonObject values = reply.value("choices").toObject();
                     for (auto it = values.begin(); it != values.end(); ++it)
                         picked.append({it.key(), it.value().toString()});
                     results.insert("choices", QVariant::fromValue(picked));
                 }
                 return {response, results};
             });
    return 2;  // unused: the reply goes later
}

// --- Screenshot --------------------------------------------------------------

uint ScreenshotAdaptor::Screenshot(const QDBusObjectPath& handle, const QString&, const QString&,
                                   const QVariantMap& options, QVariantMap&) {
    // atrium's screenshot tool, answering with the file it saved; asked to
    // be interactive, it lets the user pick first.
    const QString mode = options.value("interactive").toBool() ? "portal-interactive" : "portal";
    askShell(handle.path(), "capture.qml", {}, mode, [](const std::optional<QByteArray>& out) -> QVariantList {
        if (!out)
            return {uint(2), QVariantMap()};
        const QString uri = QString::fromUtf8(*out).trimmed();
        if (uri.isEmpty())
            return {uint(1), QVariantMap()};  // the user left
        return {uint(0), QVariantMap{{"uri", uri}}};
    });
    return 2;
}

uint ScreenshotAdaptor::PickColor(const QDBusObjectPath& handle, const QString&, const QString&,
                                  const QVariantMap&, QVariantMap&) {
    askShell(handle.path(), "capture.qml", {}, "color", [](const std::optional<QByteArray>& out) -> QVariantList {
        if (!out)
            return {uint(2), QVariantMap()};
        const QStringList rgb = QString::fromUtf8(*out).trimmed().split(' ', Qt::SkipEmptyParts);
        if (rgb.size() != 3)
            return {uint(1), QVariantMap()};
        const PortalColor color{rgb[0].toDouble(), rgb[1].toDouble(), rgb[2].toDouble()};
        return {uint(0), QVariantMap{{"color", QVariant::fromValue(color)}}};
    });
    return 2;
}

// --- Wallpaper ---------------------------------------------------------------

uint WallpaperAdaptor::SetWallpaperURI(const QDBusObjectPath&, const QString&, const QString&, const QString& uri,
                                       const QVariantMap& options) {
    // 0 done, 2 not done (1 is the user saying no).
    if (options.value("set-on").toString() == "lockscreen")
        return 2;
    QFile source(QUrl(uri).toLocalFile());
    if (source.fileName().isEmpty() || !source.open(QIODevice::ReadOnly))
        return 2;
    const QByteArray data = source.readAll();
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/atrium/wallpapers";
    const QString suffix = QFileInfo(source.fileName()).suffix();
    // Named by what's in it: the same picture twice is one file.
    const QString name = QCryptographicHash::hash(data, QCryptographicHash::Sha1).toHex().left(16) +
                         (suffix.isEmpty() ? "" : "." + suffix);
    QFile copy(dir + "/" + name);
    if (!QDir().mkpath(dir) || (!copy.exists() && (!copy.open(QIODevice::WriteOnly) || copy.write(data) != data.size())))
        return 2;
    Compositor::instance()->setSetting("appearance.wallpaper", copy.fileName());
    return 0;
}

// --- Inhibit -----------------------------------------------------------------

InhibitRequest::InhibitRequest(const QString& path, QDBusUnixFileDescriptor lock, QObject* parent)
    : QObject(parent), path_(path), lock_(std::move(lock)) {
    bus().registerObject(path, this, QDBusConnection::ExportAllSlots);
}

InhibitRequest::~InhibitRequest() {
    bus().unregisterObject(path_);
}

void InhibitRequest::Close() {
    deleteLater();  // and with it the lock
}

void InhibitAdaptor::Inhibit(const QDBusObjectPath& handle, const QString& app, const QString&, uint flags,
                             const QVariantMap& options) {
    // Flags: 1 logout, 2 switching user, 4 suspend, 8 idle.
    QDBusUnixFileDescriptor lock;
    if (flags & (4 | 8)) {
        QString why = options.value("reason").toString();
        if (why.isEmpty())
            why = QStringLiteral("Asked through the portal");
        QDBusMessage call = QDBusMessage::createMethodCall("org.freedesktop.login1", "/org/freedesktop/login1",
                                                           "org.freedesktop.login1.Manager", "Inhibit");
        call << QStringLiteral("idle") << appKey(app) << why << QStringLiteral("block");
        const QDBusMessage reply = QDBusConnection::systemBus().call(call);
        if (reply.type() == QDBusMessage::ReplyMessage && !reply.arguments().isEmpty())
            lock = reply.arguments().first().value<QDBusUnixFileDescriptor>();
    }
    new InhibitRequest(handle.path(), std::move(lock), this);
}

uint InhibitAdaptor::CreateMonitor(const QDBusObjectPath&, const QDBusObjectPath& session, const QString& app,
                                   const QString&, QVariantMap&) {
    auto* s = new PortalSession(session.path(), app, PortalBackend::instance());
    PortalBackend::instance()->addSession(s);
    // After the reply, which makes the session the app's.
    QTimer::singleShot(0, this, [this, session] {
        emit StateChanged(session, {{"screensaver-active", false}, {"session-state", uint(1)}});
    });
    return 0;
}

// --- ScreenCast --------------------------------------------------------------

namespace {

const QString kRestoreVendor = QStringLiteral("atrium");

// A source's {output} or {window} for screencast.start, from the chooser's
// pick or a restored one; empty if it isn't there (any more).
QVariantMap castTarget(const QString& kind, const QString& name, bool byApp) {
    Compositor* c = Compositor::instance();
    if (kind == "screen") {
        for (const QVariant& o : c->outputs())
            if (o.toMap().value("name").toString() == name)
                return {{"output", name}};
        return {};
    }
    for (const QVariant& v : c->windows()) {
        const QVariantMap w = v.toMap();
        if ((byApp ? w.value("app_id") : w.value("identifier")).toString() == name)
            return {{"window", w.value("id")}, {"app_id", w.value("app_id")}};
    }
    return {};
}

// The lines the chooser takes (share_core.hpp), as xdg-desktop-portal-wlr
// wrote them.
QByteArray chooserInput(uint types) {
    QByteArray out;
    Compositor* c = Compositor::instance();
    if (types & 1)
        for (const QVariant& v : c->outputs()) {
            const QVariantMap o = v.toMap();
            if (o.value("enabled").toBool())
                out += ("Monitor: " + o.value("name").toString() + " " + o.value("description").toString() + "\n")
                           .toUtf8();
        }
    if (types & 2)
        for (const QVariant& v : c->windows()) {
            const QVariantMap w = v.toMap();
            const QString id = w.value("identifier").toString();
            if (!id.isEmpty())
                out += ("Window: " + w.value("title").toString().replace('\n', ' ') + " (" + id + ")\n").toUtf8();
        }
    return out;
}

} // namespace

ScreenCastAdaptor::ScreenCastAdaptor(PortalBackend* parent) : QDBusAbstractAdaptor(parent) {
    // A stream whose screen or window went ends its session; so does atrium
    // going (every stream with it).
    connect(Compositor::instance(), &Compositor::screencastEnded, this, [this](qint64 stream) {
        for (auto it = casts_.begin(); it != casts_.end(); ++it)
            if (it->streams.contains(stream)) {
                const QString path = it.key();
                it->streams.removeAll(stream);
                if (PortalSession* s = PortalBackend::instance()->session(path))
                    s->end();
                return;
            }
    });
    connect(Compositor::instance(), &Compositor::connectedChanged, this, [this] {
        if (Compositor::instance()->connected())
            return;
        for (const QString& path : casts_.keys()) {
            casts_[path].streams.clear();
            if (PortalSession* s = PortalBackend::instance()->session(path))
                s->end();
        }
    });
}

void ScreenCastAdaptor::end(const QString& session) {
    const Cast cast = casts_.take(session);
    for (qint64 stream : cast.streams)
        Compositor::instance()->screencastStop(stream);
}

uint ScreenCastAdaptor::CreateSession(const QDBusObjectPath&, const QDBusObjectPath& session, const QString& app,
                                      const QVariantMap&, QVariantMap& results) {
    auto* s = new PortalSession(session.path(), app, PortalBackend::instance());
    PortalBackend::instance()->addSession(s);
    casts_.insert(session.path(), Cast{});
    const QString path = session.path();
    connect(s, &QObject::destroyed, this, [this, path] { end(path); });
    results.insert("session_id", path);
    return 0;
}

uint ScreenCastAdaptor::SelectSources(const QDBusObjectPath&, const QDBusObjectPath& session, const QString&,
                                      const QVariantMap& options, QVariantMap&) {
    auto it = casts_.find(session.path());
    if (it == casts_.end())
        return 2;
    if (options.contains("types"))
        it->types = options.value("types").toUInt() & sourceTypes();
    if (!it->types)
        it->types = 1;
    if (options.contains("cursor_mode"))
        it->cursor = options.value("cursor_mode").toUInt();
    it->persist = options.value("persist_mode").toUInt();
    if (options.contains("restore_data")) {
        PortalRestore r;
        options.value("restore_data").value<QDBusArgument>() >> r;
        if (r.vendor == kRestoreVendor && r.version == 1)
            it->restore = qdbus_cast<QVariantMap>(r.data.variant());
    }
    return 0;
}

uint ScreenCastAdaptor::Start(const QDBusObjectPath& handle, const QDBusObjectPath& session, const QString&,
                              const QString&, const QVariantMap&, QVariantMap&) {
    auto it = casts_.find(session.path());
    if (it == casts_.end())
        return 2;
    PortalBackend* backend = PortalBackend::instance();
    const QDBusMessage call = backend->delayReply();
    const QString path = session.path();
    auto* request = new PortalRequest(handle.path(), backend);
    auto answer = [call, request](uint response, const QVariantMap& results) {
        if (request->property("answered").toBool())
            return;
        request->setProperty("answered", true);
        bus().send(call.createReply(QVariantList{response, results}));
        request->deleteLater();
    };

    // The source picked: its stream, then the answer.
    auto cast = [this, path, answer](const QString& kind, const QVariantMap& target) {
        auto c = casts_.find(path);
        if (c == casts_.end() || target.isEmpty())
            return answer(target.isEmpty() ? 1 : 2, {});
        QVariantMap fields = target;
        fields.remove("app_id");
        fields.insert("cursor", c->cursor == 4 ? QVariant("metadata") : QVariant(c->cursor == 2));
        Compositor::instance()->screencastStart(fields, [this, path, kind, target, answer](const QJsonObject& reply) {
            auto c = casts_.find(path);
            if (!reply.value("ok").toBool() || c == casts_.end())
                return answer(2, {});
            const QJsonObject r = reply.value("result").toObject();
            const qint64 stream = r.value("stream").toInteger();
            c->streams.append(stream);
            QVariantMap props{{"source_type", uint(kind == "window" ? 2 : 1)}};
            // Where it is and how big, in the desktop's logical pixels.
            int x = 0, y = 0, w = r.value("width").toInt(), h = r.value("height").toInt();
            if (kind == "area") {
                // What the stream holds: the area cut to the screen its middle is on.
                const QVariantMap a = target.value("region").toMap();
                x = a.value("x").toInt(), y = a.value("y").toInt();
                w = a.value("width").toInt(), h = a.value("height").toInt();
            }
            if (kind == "screen")
                for (const QVariant& o : Compositor::instance()->outputs())
                    if (o.toMap().value("name") == target.value("output")) {
                        const QVariantMap g = o.toMap().value("geometry").toMap();
                        x = g.value("x").toInt(), y = g.value("y").toInt();
                        w = g.value("width").toInt(), h = g.value("height").toInt();
                    }
            QDBusArgument position, size;
            position.beginStructure();
            position << x << y;
            position.endStructure();
            size.beginStructure();
            size << w << h;
            size.endStructure();
            if (kind != "window")
                props.insert("position", QVariant::fromValue(position));
            props.insert("size", QVariant::fromValue(size));
            props.insert("id", QString::number(stream));
            QVariantMap results{{"streams", QVariant::fromValue(PortalStreams{{uint(r.value("node").toInt()), props}})}};
            if (c->persist && kind != "area") {
                // Next time without asking: the same screen, or a window of the same app.
                const QString name = kind == "screen" ? target.value("output").toString()
                                                      : target.value("app_id").toString();
                const PortalRestore restore{kRestoreVendor, 1,
                                            QDBusVariant(QVariantMap{{"kind", kind}, {"name", name}})};
                results.insert("persist_mode", c->persist);
                results.insert("restore_data", QVariant::fromValue(restore));
            }
            answer(0, results);
        });
    };

    // An earlier pick that's still there: no need to ask.
    const QString kind = it->restore.value("kind").toString();
    if (!kind.isEmpty() && (kind == "screen" ? (it->types & 1) : (it->types & 2))) {
        const QVariantMap target = castTarget(kind, it->restore.value("name").toString(), true);
        if (!target.isEmpty()) {
            cast(kind, target);
            return 2;
        }
    }

    auto* chooser = new QProcess(request);
    QObject::connect(request, &PortalRequest::closed, chooser, [chooser] { chooser->kill(); });
    QObject::connect(chooser, &QProcess::finished, request, [request, chooser, cast, answer](int code,
                                                                                         QProcess::ExitStatus st) {
        const QByteArray out = chooser->readAllStandardOutput();
        if (st == QProcess::NormalExit && code == 0 && out.trimmed() == "Area:") {
            // An area next, picked with the screenshot tool: "x y width height".
            auto* picker = new QProcess(request);
            QObject::connect(request, &PortalRequest::closed, picker, [picker] { picker->kill(); });
            QObject::connect(picker, &QProcess::finished, request, [picker, cast, answer] {
                const QStringList v = QString::fromUtf8(picker->readAllStandardOutput()).simplified().split(' ');
                if (v.size() != 4)
                    return answer(1, {});
                cast("area", {{"region", QVariantMap{{"x", v[0].toInt()}, {"y", v[1].toInt()},
                                                     {"width", v[2].toInt()}, {"height", v[3].toInt()}}}});
            });
            QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
            env.insert("ATRIUM_CAPTURE_MODE", "area");
            picker->setProcessEnvironment(env);
            // Once the chooser has gone (it zooms out over 300 ms): the picker
            // freezes the screen as it starts.
            QTimer::singleShot(350, picker, [picker] { picker->start(shellProgram(), {shellFile("capture.qml")}); });
            return;
        }
        const auto picked = share::parse(out.toStdString());
        if (st != QProcess::NormalExit || code != 0 || picked.empty())
            return answer(1, {});  // the user said no
        const share::Source& p = picked.front();
        const QString kind = p.kind == share::Source::Kind::Screen ? "screen" : "window";
        cast(kind, castTarget(kind, QString::fromStdString(p.name), false));
    });
    QObject::connect(chooser, &QProcess::errorOccurred, request, [answer](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart)
            answer(2, {});
    });
    chooser->start(shellProgram(), {shellFile("share.qml")});
    chooser->write(chooserInput(it->types));
    chooser->closeWriteChannel();
    return 2;
}

} // namespace atrium
