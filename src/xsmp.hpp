#pragma once
// XSMP, the X11 session protocol (libSM over ICE), for the X11 apps that
// save through a session manager (Qt and KDE apps on X11, LibreOffice):
// logging out asks each to save first, an app may ask "Save changes?" (one
// at a time), and one that says Cancel cancels the logout, as ksmserver
// does. Apps find it through SESSION_MANAGER; connections are checked
// against a cookie in ~/.ICEauthority (the socket is in /tmp, open to
// every local user), removed again when atrium goes.

#include "common.hpp"

#include <functional>
#include <memory>
#include <string>
#include <vector>

struct _IceConn;

namespace atrium {

class Server;

class Xsmp {
public:
    explicit Xsmp(Server& server);
    ~Xsmp();
    Xsmp(const Xsmp&) = delete;
    Xsmp& operator=(const Xsmp&) = delete;

    bool ok() const { return !address_.empty(); }
    // For SESSION_MANAGER.
    const std::string& address() const { return address_; }
    size_t clients() const;

    // Logging out: every client saves (those that ask, interacting one at
    // a time); `done` with "" when all have, or with the app that
    // cancelled. One that never answers is given up on after a while
    // (unless it is asking the user something).
    void save_all(std::function<void(const std::string& holdout)> done);
    // The logout was called off: they carry on.
    void cancel();
    // The session ends: they quit.
    void die();

    struct Client;

private:
    struct Listener;
    void accept(Listener& l);
    void watch(_IceConn* conn, bool opening);
    void next_interaction();
    void check_saved();
    void finish_saving(const std::string& holdout);

    friend struct XsmpCallbacks;

    Server& server_;
    std::string address_;
    std::vector<std::unique_ptr<Listener>> listeners_;
    std::vector<std::unique_ptr<Client>> clients_;
    std::vector<std::pair<_IceConn*, wl_event_source*>> connections_;
    // Saving for a logout.
    bool saving_ = false;
    bool shutting_down_ = false;  // asked to save for a logout, not called off
    Client* interacting_ = nullptr;
    std::vector<Client*> waiting_;  // asked to interact, in turn
    std::function<void(const std::string&)> done_;
    wl_event_source* timeout_ = nullptr;
    std::vector<std::string> auth_ids_;  // our ~/.ICEauthority entries' network ids
};

} // namespace atrium
