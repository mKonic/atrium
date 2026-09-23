#pragma once
#include "listener.hpp"

namespace atrium {

class Server;

// ext-session-lock-v1: while a lock exists nothing but the lock surfaces gets
// input or is visible. If the locker dies without unlocking, the session stays
// locked (the lock background keeps covering everything).
class SessionLock {
public:
    SessionLock(Server& server, wlr_session_lock_v1* lock);
    ~SessionLock();
    SessionLock(const SessionLock&) = delete;
    SessionLock& operator=(const SessionLock&) = delete;

    Server& server;
    wlr_session_lock_v1* const wlr;
    wlr_scene_tree* tree = nullptr;

private:
    void new_surface(wlr_session_lock_surface_v1* surface);
    void finish(bool unlocked);

    Listener<wlr_session_lock_surface_v1> new_surface_;
    Listener<> unlock_;
    Listener<> destroy_;
};

} // namespace atrium
