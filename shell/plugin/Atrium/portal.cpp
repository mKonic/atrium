#include "portal.hpp"

#include "compositor.hpp"

#include <QDBusConnection>
#include <QDBusMetaType>
#include <QColor>
#include <QDateTime>

#include <algorithm>

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
    new GlobalShortcutsAdaptor(this);
    new SettingsAdaptor(this);
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
        out.append({id, {{"description", id}, {"trigger_description", PortalBackend::describe(keys)}}});
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

} // namespace atrium
