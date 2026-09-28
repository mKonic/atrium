#pragma once
#include "listener.hpp"
#include "registry.hpp"
#include "wl/resource.hpp"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace atrium {

class Server;
class View;
namespace wl {
class XdgSessionManagerV1;
class XdgSessionV1;
class XdgToplevelSessionV1;
}

// xdg-session-management-v1: an app that asks for its windows back gets
// them where they were, by the names it gives them, across its own restarts
// and atrium's. Kept in the registry ("sessions"); a session unused for half
// a year is dropped.
class SessionManagement {
public:
    explicit SessionManagement(Server& server);
    ~SessionManagement();
    SessionManagement(const SessionManagement&) = delete;
    SessionManagement& operator=(const SessionManagement&) = delete;

    // How `view` comes back, if its app asked for it to be restored and its
    // session knew it.
    const SessionWindow* restoring(const View* view) const;
    // The window moved, resized or changed state: remember it (soon, once
    // it holds still).
    void view_changed(const View* view);
    // Now, before it goes.
    void view_unmapping(const View* view);

private:
    struct ToplevelSession;
    struct Session {
        wl::Weak<wl::XdgSessionV1> resource;
        wl_client* client;
        std::string id;
        std::vector<ToplevelSession*> toplevels;
    };
    struct ToplevelSession {
        Session* session;  // null once inert
        wl::Weak<wl::XdgToplevelSessionV1> resource;
        wlr_xdg_toplevel* toplevel;
        std::string name;
        std::optional<SessionWindow> restore;
        Listener<> toplevel_destroy;
    };

    void get_session(wl::XdgSessionManagerV1* manager, uint32_t id, uint32_t reason, const char* session_id);
    void remove_toplevel(Session* session, const char* name);
    void rename(ToplevelSession* t, const char* name);
    void session_gone(Session* session);
    void track(Session* session, wl::XdgSessionV1* resource, uint32_t id, wl_resource* toplevel, const char* name,
               bool restore);
    void make_inert(Session* session);
    void save(const ToplevelSession& t);
    ToplevelSession* find(const View* view) const;
    View* view_of(const ToplevelSession& t) const;

    Server& server_;
    std::unique_ptr<wl::Global> global_;
    std::vector<wl::Weak<wl::XdgSessionManagerV1>> managers_;
    std::vector<ToplevelSession*> toplevels_;  // including inert ones
    std::vector<Session*> sessions_;
    std::vector<const View*> dirty_;
    wl_event_source* save_timer_ = nullptr;
};

} // namespace atrium
