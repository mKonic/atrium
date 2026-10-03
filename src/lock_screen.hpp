#pragma once
#include "common.hpp"

#include <sys/types.h>

namespace atrium {

class Server;

// atrium's lock screen: atrium-shell running lock.qml with the atrium-lock
// Qt shell integration, an ext-session-lock client like any other locker.
// It must not fail open: the session stays locked if it dies (the protocol
// keeps it so), and it is started again while the session is locked,
// rendering in software after a few failures (as kscreenlocker does).
class LockScreen {
public:
    explicit LockScreen(Server& server);
    ~LockScreen();
    LockScreen(const LockScreen&) = delete;
    LockScreen& operator=(const LockScreen&) = delete;

    // Locks the session, unless something already holds the lock.
    void lock();
    // Locks, and once the session is locked asks atrium-login for a greeter
    // beside it (Switch User); never with the session still showing.
    void switch_user();
    bool running() const { return pid_ > 0; }
    // logind's Unlock: ends a lock our lock screen holds (or one whose locker
    // died); another locker's lock is that locker's to end.
    void unlock();

private:
    void start();
    void exited(int status);

    Server& server_;
    pid_t pid_ = -1;
    int failures_ = 0;
    bool stopping_ = false;  // unlocked from outside, ours told to go
    bool relock_ = false;    // and asked to lock again meanwhile
    int pipe_[2] = {-1, -1};
    int watch_ = -1;
    wl_event_source* pipe_source_ = nullptr;
    wl_event_source* retry_ = nullptr;
    wl_event_source* switch_wait_ = nullptr;
    int switch_tries_ = 0;
};

} // namespace atrium
