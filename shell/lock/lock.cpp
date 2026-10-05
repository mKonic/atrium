#include "lock.hpp"

#include <QtWaylandClient/private/qwaylanddisplay_p.h>
#include <QtWaylandClient/private/qwaylandscreen_p.h>
#include <QtWaylandClient/private/qwaylandshellintegrationplugin_p.h>
#include <QtWaylandClient/private/qwaylandwindow_p.h>

#include <QGuiApplication>

#include <cstdlib>

namespace atrium::lock {

using namespace QtWaylandClient;

SessionLock::SessionLock(struct ::ext_session_lock_v1* lock) : QtWayland::ext_session_lock_v1(lock) {}

SessionLock::~SessionLock() {
    // Gone without unlocking: the session stays locked (the protocol says so).
    if (!unlocked_ && isInitialized())
        destroy();
}

void SessionLock::unlock() {
    if (unlocked_ || !locked_)
        return;
    unlocked_ = true;
    unlock_and_destroy();
    // On its way before the process can go.
    if (auto* app = qGuiApp->nativeInterface<QNativeInterface::QWaylandApplication>())
        wl_display_flush(app->display());
}

void SessionLock::ext_session_lock_v1_locked() {
    locked_ = true;
    emit lockedChanged();
}

// Refused (another locker holds the session): nothing for this one to do.
void SessionLock::ext_session_lock_v1_finished() {
    qWarning("atrium-lock: the compositor refused the lock");
    if (!locked_)
        std::exit(2);
}

Integration::Integration() : QWaylandShellIntegrationTemplate<Integration>(1) {}

Integration::~Integration() {
    if (isInitialized())
        destroy();
}

bool Integration::initialize(QWaylandDisplay* display) {
    if (!QWaylandShellIntegrationTemplate<Integration>::initialize(display))
        return false;
    lock_ = new SessionLock(lock());
    lock_->setParent(qApp);
    qApp->setProperty("atriumSessionLock", QVariant::fromValue(static_cast<QObject*>(lock_)));
    return true;
}

QWaylandShellSurface* Integration::createShellSurface(QWaylandWindow* window) {
    return new Surface(lock_, window);
}

Surface::Surface(SessionLock* lock, QWaylandWindow* window) : QWaylandShellSurface(window) {
    QWaylandScreen* screen = window->waylandScreen();
    init(lock->get_lock_surface(window->wlSurface(), screen ? screen->output() : nullptr));
}

Surface::~Surface() {
    if (isInitialized())
        destroy();
}

void Surface::ext_session_lock_surface_v1_configure(uint32_t serial, uint32_t width, uint32_t height) {
    ack_configure(serial);
    size_ = QSize(int(width), int(height));
    if (!configured_) {
        configured_ = true;
        window()->resizeFromApplyConfigure(size_);
        window()->updateExposure();
    } else {
        window()->applyConfigureWhenPossible();
    }
}

void Surface::applyConfigure() {
    window()->resizeFromApplyConfigure(size_);
}

class Plugin : public QWaylandShellIntegrationPlugin {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID QWaylandShellIntegrationFactoryInterface_iid FILE "atrium-lock.json")

public:
    QWaylandShellIntegration* create(const QString& key, const QStringList&) override {
        return key == QLatin1String("atrium-lock") ? new Integration : nullptr;
    }
};

} // namespace atrium::lock

#include "lock.moc"
