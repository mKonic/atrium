#include "portal.hpp"

#include "compositor.hpp"
#include "paths.hpp"

#include <QColor>
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
    new GlobalShortcutsAdaptor(this);
    new SettingsAdaptor(this);
    new WallpaperAdaptor(this);
    new AccessAdaptor(this);
    new ScreenshotAdaptor(this);
    new InhibitAdaptor(this);
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
    const bool wanted = namespaces.isEmpty() || std::ranges::any_of(namespaces, [](const QString& n) {
        return n == kAppearance || (n.endsWith('*') && kAppearance.startsWith(n.chopped(1)));
    });
    if (wanted)
        out.insert(kAppearance, appearance());
    return out;
}

QDBusVariant SettingsAdaptor::Read(const QString& ns, const QString& key) {
    const QVariantMap a = appearance();
    if (ns != kAppearance || !a.contains(key)) {
        static_cast<PortalBackend*>(parent())->fail("org.freedesktop.portal.Error.NotFound", "No such setting");
        return {};
    }
    return QDBusVariant(a.value(key));
}

// --- Access ------------------------------------------------------------------

namespace {

// An installed file, else the source tree's (running from the build tree).
QString shellFile(const QString& name) {
    const QString installed = QStringLiteral(ATRIUM_DATADIR "/shell/") + name;
    return QFileInfo::exists(installed) ? installed : QStringLiteral(ATRIUM_SOURCE_DIR "/shell/") + name;
}

QString shellProgram() {
    const QString installed = QStringLiteral(ATRIUM_BINDIR "/atrium-shell");
    return QFileInfo::exists(installed) ? installed : QStringLiteral(ATRIUM_BUILD_DIR "/shell/host/atrium-shell");
}

} // namespace

PortalRequest::PortalRequest(const QString& path, QObject* parent) : QObject(parent), path_(path) {
    bus().registerObject(path, this, QDBusConnection::ExportAllSlots);
}

PortalRequest::~PortalRequest() {
    bus().unregisterObject(path_);
}

namespace {

// What goes back to the portal, from a dialog's stdout (nothing when it
// failed or the portal closed it first).
using Answer = std::function<QVariantList(const std::optional<QByteArray>& out)>;

// Runs a shell file (a dialog) in its own atrium-shell for the call being
// answered, with `input` on its stdin and `mode` in ATRIUM_CAPTURE_MODE,
// until it exits or the portal closes the request.
void askShell(const QString& handle, const QString& file, const QByteArray& input, const QString& mode,
              Answer answer) {
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
    dialog->closeWriteChannel();
}

} // namespace

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

} // namespace atrium
