#pragma once
// atrium's own logind session, over the system bus: its Lock and Unlock
// (loginctl lock-session, and a login manager switching back to a session
// it just checked the password for), the lock before sleep, and the
// LockedHint others read. Without libsystemd (or a logind session) it
// does nothing.
#include "wlr.hpp"

#include <memory>
#include <string>

namespace atrium {

class Server;

class Logind {
public:
    explicit Logind(Server& server);
    ~Logind();
    Logind(const Logind&) = delete;
    Logind& operator=(const Logind&) = delete;

    // What `loginctl show-session -p LockedHint` says.
    void set_locked_hint(bool locked);
    // The settings changed: take the sleep inhibitor, or let it go.
    void reconfigure();

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

} // namespace atrium
