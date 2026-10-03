#include "app_menu.hpp"

#include "compositor.hpp"
#include "dbus_menu.hpp"

#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusServiceWatcher>

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
}

void AppMenu::follow() {
    const QVariantMap w = Compositor::instance()->focusedWindow().toMap();
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
    if (menus == menus_)
        return;
    menus_ = menus;
    emit changed();
}

QVariantList AppMenu::items(int id) const {
    return dbusmenu::layout(service_, path_, id);
}

void AppMenu::trigger(int id) const {
    dbusmenu::trigger(service_, path_, id);
}

} // namespace atrium
