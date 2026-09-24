#pragma once
#include "listener.hpp"
#include "registry.hpp"

#include <optional>
#include <string>
#include <vector>

namespace atrium {

class Server;
class View;

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
        SessionManagement* owner;
        wl_resource* resource;
        wl_client* client;
        std::string id;
        std::vector<ToplevelSession*> toplevels;
    };
    struct ToplevelSession {
        Session* session;  // null once inert
        wl_resource* resource;
        wlr_xdg_toplevel* toplevel;
        std::string name;
        std::optional<SessionWindow> restore;
        Listener<> toplevel_destroy;
    };

    static void bind(wl_client* client, void* data, uint32_t version, uint32_t id);
    static void get_session(wl_client* client, wl_resource* manager, uint32_t id, uint32_t reason,
                            const char* session_id);
    static void add_toplevel(wl_client* client, wl_resource* session, uint32_t id, wl_resource* toplevel,
                             const char* name);
    static void restore_toplevel(wl_client* client, wl_resource* session, uint32_t id, wl_resource* toplevel,
                                 const char* name);
    static void remove_toplevel(wl_client* client, wl_resource* session, const char* name);
    static void remove_session(wl_client* client, wl_resource* session);
    static void rename(wl_client* client, wl_resource* toplevel_session, const char* name);
    static void destroy_resource(wl_client* client, wl_resource* resource);
    static void manager_gone(wl_resource* resource);
    static void session_gone(wl_resource* resource);
    static void toplevel_session_gone(wl_resource* resource);
    ToplevelSession* track(Session* session, uint32_t id, wl_resource* toplevel, const char* name, bool restore);
    void make_inert(Session* session);
    void save(const ToplevelSession& t);
    ToplevelSession* find(const View* view) const;
    View* view_of(const ToplevelSession& t) const;

    Server& server_;
    wl_global* global_ = nullptr;
    std::vector<wl_resource*> managers_;
    std::vector<Session*> sessions_;
    std::vector<const View*> dirty_;
    wl_event_source* save_timer_ = nullptr;
};

} // namespace atrium
