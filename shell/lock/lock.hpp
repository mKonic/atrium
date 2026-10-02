#pragma once
// A Qt Wayland shell integration ("atrium-lock") that turns every window of
// the process into an ext-session-lock-v1 lock surface on its screen. Loading
// it locks the session (the process must be allowed the lock global: atrium
// lets the lock screen it starts have it). The screen stays locked until
// unlock() is called, also if the process dies.
//
// The lock is reachable from QML plugins as qApp->property("atriumSessionLock")
// (a SessionLock*): invoke "unlock" once the user has authenticated.
#include "qwayland-ext-session-lock-v1.h"

#include <QtWaylandClient/private/qwaylandshellintegration_p.h>
#include <QtWaylandClient/private/qwaylandshellsurface_p.h>

#include <QObject>
#include <QSize>

namespace atrium::lock {

class SessionLock : public QObject, public QtWayland::ext_session_lock_v1 {
    Q_OBJECT
    Q_PROPERTY(bool locked READ locked NOTIFY lockedChanged)

public:
    explicit SessionLock(struct ::ext_session_lock_v1* lock);
    ~SessionLock() override;
    bool locked() const { return locked_; }
    Q_INVOKABLE void unlock();

signals:
    void lockedChanged();

protected:
    void ext_session_lock_v1_locked() override;
    void ext_session_lock_v1_finished() override;

private:
    bool locked_ = false;
    bool unlocked_ = false;
};

class Integration : public QtWaylandClient::QWaylandShellIntegrationTemplate<Integration>,
                    public QtWayland::ext_session_lock_manager_v1 {
public:
    Integration();
    ~Integration() override;
    bool initialize(QtWaylandClient::QWaylandDisplay* display) override;
    QtWaylandClient::QWaylandShellSurface* createShellSurface(QtWaylandClient::QWaylandWindow* window) override;

private:
    SessionLock* lock_ = nullptr;
};

class Surface : public QtWaylandClient::QWaylandShellSurface, public QtWayland::ext_session_lock_surface_v1 {
public:
    Surface(SessionLock* lock, QtWaylandClient::QWaylandWindow* window);
    ~Surface() override;
    // Nothing is drawn before the compositor says how big.
    bool isExposed() const override { return configured_; }
    // No empty commit to set up the role (xdg-shell's way): a lock surface
    // may not commit before its first configure is acked.
    bool commitSurfaceRole() const override { return false; }
    void applyConfigure() override;

protected:
    void ext_session_lock_surface_v1_configure(uint32_t serial, uint32_t width, uint32_t height) override;

private:
    QSize size_;
    bool configured_ = false;
};

} // namespace atrium::lock
