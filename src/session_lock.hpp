#pragma once
#include "listener.hpp"
#include "scene/scene.hpp"
#include "wl/session_lock.hpp"

namespace atrium {

class Server;

// ext-session-lock-v1: while a lock exists nothing but the lock surfaces gets
// input or is visible. If the locker dies without unlocking, the session stays
// locked (the lock background keeps covering everything).
class SessionLock {
public:
    SessionLock(Server& server, wl::Lock* lock);
    ~SessionLock();
    SessionLock(const SessionLock&) = delete;
    SessionLock& operator=(const SessionLock&) = delete;

    Server& server;
    wl::Lock* const lock;
    scene::Tree* tree = nullptr;

    // Unlocked from outside the locker (logind's Unlock).
    void unlock() { finish(true); }
    // The session after a lock ends; with no lock (its locker died), also
    // the way out of the locked state.
    static void ended(Server& server, bool unlocked);

private:
    void new_surface(wl::LockSurface* surface);
    void finish(bool unlocked);

    wl::Connection new_surface_, unlock_, destroy_;
};

} // namespace atrium
