#include "app_menu.hpp"

#include "compositor.hpp"
#include "dbus_menu.hpp"
#include "default_apps.hpp"
#include "desktop_entries.hpp"

#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusServiceWatcher>
#include <QStandardPaths>
#include <QUrl>

#include <functional>

namespace atrium {

namespace {

const QString kRegistrar = QStringLiteral("com.canonical.AppMenu.Registrar");
const QString kRegistrarPath = QStringLiteral("/com/canonical/AppMenu/Registrar");
const QString kMenu = QStringLiteral("com.canonical.dbusmenu");

} // namespace

// --- registrar -------------------------------------------------------------------

AppMenuRegistrar::AppMenuRegistrar(QObject* parent) : QObject(parent) {
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.registerObject(kRegistrarPath, this,
                            QDBusConnection::ExportScriptableSlots | QDBusConnection::ExportScriptableSignals) ||
        !bus.registerService(kRegistrar)) {
        qWarning("appmenu: another registrar has %s", qPrintable(kRegistrar));
        return;
    }
    watcher_ = new QDBusServiceWatcher(this);
    watcher_->setConnection(bus);
    watcher_->setWatchMode(QDBusServiceWatcher::WatchForUnregistration);
    // An app gone: its windows' menus with it.
    connect(watcher_, &QDBusServiceWatcher::serviceUnregistered, this, [this](const QString& service) {
        watcher_->removeWatchedService(service);
        bool any = false;
        for (auto it = windows_.begin(); it != windows_.end();)
            if (it->service == service) {
                emit WindowUnregistered(it.key());
                it = windows_.erase(it);
                any = true;
            } else {
                ++it;
            }
        if (any)
            emit changed();
    });
}

std::pair<QString, QString> AppMenuRegistrar::menuFor(uint window) const {
    const Registered r = windows_.value(window);
    return {r.service, r.path};
}

std::pair<QString, QString> AppMenuRegistrar::menuForPid(uint pid) const {
    const Registered* last = nullptr;
    for (const Registered& r : windows_)
        if (pid && r.pid == pid && (!last || r.order > last->order))
            last = &r;
    return last ? std::pair{last->service, last->path} : std::pair<QString, QString>{};
}

void AppMenuRegistrar::RegisterWindow(uint windowId, const QDBusObjectPath& menuObjectPath) {
    const QString service = calledFromDBus() ? message().service() : QString();
    const uint pid = service.isEmpty() ? 0 : connection().interface()->servicePid(service).value();
    windows_.insert(windowId, {service, menuObjectPath.path(), pid, next_++});
    if (watcher_ && !service.isEmpty() && !watcher_->watchedServices().contains(service))
        watcher_->addWatchedService(service);
    emit WindowRegistered(windowId, service, menuObjectPath);
    emit changed();
}

void AppMenuRegistrar::UnregisterWindow(uint windowId) {
    if (windows_.remove(windowId)) {
        emit WindowUnregistered(windowId);
        emit changed();
    }
}

QString AppMenuRegistrar::GetMenuForWindow(uint windowId, QDBusObjectPath& menuObjectPath) {
    const Registered r = windows_.value(windowId);
    menuObjectPath = QDBusObjectPath(r.path.isEmpty() ? QStringLiteral("/") : r.path);
    return r.service;
}

// --- the menu bar's menus -----------------------------------------------------------

AppMenu::AppMenu(QObject* parent) : QObject(parent), registrar_(new AppMenuRegistrar(this)) {
    settle_.setSingleShot(true);
    settle_.setInterval(50);
    connect(&settle_, &QTimer::timeout, this, &AppMenu::reload);
    connect(Compositor::instance(), &Compositor::windowsChanged, this, &AppMenu::follow);
    connect(registrar_, &AppMenuRegistrar::changed, this, &AppMenu::follow);
    follow();
    reload();  // the menus atrium has, though nothing changed yet
}

void AppMenu::follow() {
    const QVariantMap w = Compositor::instance()->focusedWindow().toMap();
    if (const bool desktop = w.isEmpty(); desktop != desktop_) {
        desktop_ = desktop;
        reload();
    }
    QString service, path;
    if (const QVariantMap m = w.value("menu").toMap(); !m.isEmpty()) {
        service = m.value("service").toString();
        path = m.value("path").toString();
    } else if (w.value("x11_window").isValid() && !w.value("x11_window").isNull()) {
        std::tie(service, path) = registrar_->menuFor(w.value("x11_window").toUInt());
    } else if (w.value("pid").toUInt() > 0) {
        std::tie(service, path) = registrar_->menuForPid(w.value("pid").toUInt());
    }
    if (service == service_ && path == path_)
        return;
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!service_.isEmpty()) {
        bus.disconnect(service_, path_, kMenu, "LayoutUpdated", this, SLOT(layoutUpdated()));
        bus.disconnect(service_, path_, kMenu, "ItemsPropertiesUpdated", this, SLOT(layoutUpdated()));
    }
    service_ = service;
    path_ = path;
    // The app changes its menus (a document opened): they follow.
    if (!service_.isEmpty()) {
        bus.connect(service_, path_, kMenu, "LayoutUpdated", this, SLOT(layoutUpdated()));
        bus.connect(service_, path_, kMenu, "ItemsPropertiesUpdated", this, SLOT(layoutUpdated()));
    }
    reload();
}

void AppMenu::layoutUpdated() {
    settle_.start();
}

void AppMenu::reload() {
    QVariantList menus;
    for (const QVariant& v : dbusmenu::layout(service_, path_)) {
        const QVariantMap e = v.toMap();
        if (e.value("separator").toBool() || e.value("text").toString().isEmpty())
            continue;
        menus.append(QVariantMap{{"id", e.value("id")}, {"text", e.value("text")}, {"enabled", e.value("enabled")}});
    }
    exported_ = menus;
    // An app sharing no menus has atrium's Window menu; the desktop has the
    // file manager's (Finder's on a Mac).
    if (menus.isEmpty()) {
        const auto own = [](int id, const QString& text) { return QVariantMap{{"id", id}, {"text", text}, {"enabled", true}}; };
        if (desktop_ && !desktopApp().isEmpty())
            menus = {own(kFileMenu, tr("File")), own(kGoMenu, tr("Go")), own(kWindowMenu, tr("Window"))};
        else if (!desktop_)
            menus = {own(kWindowMenu, tr("Window"))};
    }
    if (menus == menus_)
        return;
    menus_ = menus;
    emit changed();
}

QVariantMap AppMenu::desktopApp() const {
    const QString id = DefaultApps::defaultFor("inode/directory");
    if (auto* e = shell::DesktopEntries::instance()->byId(id))
        return {{"id", e->id()}, {"name", e->name()}, {"icon", e->icon()}};
    return {};
}

QVariantList AppMenu::items(int id) const {
    if (id < 0)
        return ownItems(id);
    return dbusmenu::layout(service_, path_, id);
}

void AppMenu::trigger(int id) const {
    if (id < 0)
        return ownTrigger(id);
    dbusmenu::trigger(service_, path_, id);
}

namespace {

// The keys a shortcut for `action` has, as a Mac writes them ("⌘Q").
QString hintFor(const QString& action) {
    for (const QVariant& v : Compositor::instance()->shortcuts()) {
        const QVariantMap s = v.toMap();
        if (s.value("action") != action || !s.value("arg").toString().isEmpty())
            continue;
        // Modifiers in a Mac's order (⌃⌥⇧⌘), then the key.
        QString mods[4], key;
        for (const QString& part : s.value("keys").toString().split('+')) {
            if (part == "Ctrl" || part == "Control")
                mods[0] = QString::fromUtf8("⌃");
            else if (part == "Alt")
                mods[1] = QString::fromUtf8("⌥");
            else if (part == "Shift")
                mods[2] = QString::fromUtf8("⇧");
            else if (part == "Mod" || part == "Super")
                mods[3] = QString::fromUtf8("⌘");
            else
                key = part.size() == 1 ? part.toUpper() : part;
        }
        const QString text = mods[0] + mods[1] + mods[2] + mods[3] + key;
        return text;
    }
    return {};
}

QVariantMap row(int id, const QString& text, bool enabled = true, const QString& hint = {}) {
    return {{"id", id}, {"text", text}, {"enabled", enabled}, {"separator", false}, {"checked", false},
            {"submenu", false}, {"children", QVariantList{}}, {"shortcut", hint}};
}

QVariantMap separator() {
    return {{"id", 0}, {"text", QString()}, {"enabled", true}, {"separator", true}, {"checked", false},
            {"submenu", false}, {"children", QVariantList{}}, {"shortcut", QString()}};
}

// The windows that count: managed ones, not in a secret space.
QList<QVariantMap> windowsOf(const std::function<bool(const QVariantMap&)>& take) {
    QList<QVariantMap> out;
    for (const QVariant& v : Compositor::instance()->windows()) {
        const QVariantMap w = v.toMap();
        if (!w.value("secret").toBool() && take(w))
            out.push_back(w);
    }
    return out;
}

} // namespace

QVariantList AppMenu::ownItems(int menu) const {
    const QVariantMap w = Compositor::instance()->focusedWindow().toMap();
    const bool has = !w.isEmpty();
    QString app = desktopApp().value("name").toString();
    if (has) {
        const QString id = w.value("app_id").toString();
        auto* e = shell::DesktopEntries::instance()->heuristicLookup(id);
        app = e ? e->name() : id;
    }
    switch (menu) {
    case kAppMenu:
        return {row(Hide, tr("Hide %1").arg(app), has), row(HideOthers, tr("Hide Others")),
                row(ShowAll, tr("Show All")), separator(), row(Quit, tr("Quit %1").arg(app), has)};
    case kWindowMenu:
        return {row(Minimize, tr("Minimize"), has, hintFor("minimize")),
                row(Zoom, tr("Zoom"), has, hintFor("maximize")),
                row(FullScreen, w.value("fullscreen").toBool() ? tr("Exit Full Screen") : tr("Enter Full Screen"), has,
                    hintFor("fullscreen")),
                separator(), row(Close, tr("Close Window"), has, hintFor("close")), separator(),
                row(BringAll, tr("Bring All to Front"), has)};
    case kFileMenu:
        return {row(NewWindow, tr("New %1 Window").arg(app))};
    case kGoMenu:
        return {row(GoHome, tr("Home")), row(GoDesktop, tr("Desktop")), row(GoDocuments, tr("Documents")),
                row(GoDownloads, tr("Downloads")), separator(), row(GoTrash, tr("Trash"))};
    default:
        return {};
    }
}

void AppMenu::ownTrigger(int id) const {
    Compositor* c = Compositor::instance();
    const QVariantMap focused = c->focusedWindow().toMap();
    const int fid = focused.value("id").toInt();
    const QString app = focused.value("app_id").toString();
    const QString space = focused.value("space").toString();
    const QString output = focused.value("output").toString();
    const auto minimize = [c](const QVariantMap& w, bool on) { c->windowRequest(w.value("id").toInt(), "minimize", {{"value", on}}); };
    const auto open = [this](const QString& target) {
        if (auto* e = shell::DesktopEntries::instance()->byId(desktopApp().value("id").toString()))
            e->open({target});
    };
    const auto folder = [](QStandardPaths::StandardLocation l) {
        return QUrl::fromLocalFile(QStandardPaths::writableLocation(l)).toString();
    };
    switch (id) {
    case Hide:  // as a Mac's: every window of the app
        for (const QVariantMap& w : windowsOf([&](const QVariantMap& w) { return w.value("app_id") == app; }))
            minimize(w, true);
        break;
    case HideOthers:  // the other apps on this space
        for (const QVariantMap& w : windowsOf([&](const QVariantMap& w) {
                 return w.value("app_id") != app && (focused.isEmpty() || (w.value("space") == space && w.value("output") == output));
             }))
            minimize(w, true);
        break;
    case ShowAll:
        for (const QVariantMap& w : windowsOf([](const QVariantMap& w) { return w.value("minimized").toBool(); }))
            minimize(w, false);
        break;
    case Quit:
        for (const QVariantMap& w : windowsOf([&](const QVariantMap& w) { return w.value("app_id") == app; }))
            c->closeWindow(w.value("id").toInt());
        break;
    case Minimize: c->windowRequest(fid, "minimize", {{"value", true}}); break;
    case Zoom: c->windowRequest(fid, "maximize"); break;
    case FullScreen: c->windowRequest(fid, "fullscreen"); break;
    case Close: c->closeWindow(fid); break;
    case BringAll: {
        // Least recent first, so the one in front stays in front.
        QList<QVariantMap> all = windowsOf([&](const QVariantMap& w) { return w.value("app_id") == app; });
        for (auto it = all.rbegin(); it != all.rend(); ++it)
            c->focusWindow(it->value("id").toInt());
        break;
    }
    case NewWindow:
        if (auto* e = shell::DesktopEntries::instance()->byId(desktopApp().value("id").toString()))
            e->execute();
        break;
    case GoHome: open(folder(QStandardPaths::HomeLocation)); break;
    case GoDesktop: open(folder(QStandardPaths::DesktopLocation)); break;
    case GoDocuments: open(folder(QStandardPaths::DocumentsLocation)); break;
    case GoDownloads: open(folder(QStandardPaths::DownloadLocation)); break;
    case GoTrash: open("trash:///"); break;
    default: break;
    }
}

} // namespace atrium
