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
    bool running() const { return pid_ > 0; }

private:
    void start();
    void exited(int status);

    Server& server_;
    pid_t pid_ = -1;
    int failures_ = 0;
    int pipe_[2] = {-1, -1};
    int watch_ = -1;
    wl_event_source* pipe_source_ = nullptr;
    wl_event_source* retry_ = nullptr;
};

} // namespace atrium
