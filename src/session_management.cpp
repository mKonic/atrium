#include "session_management.hpp"

#include "server.hpp"
#include "view.hpp"

#include "xdg-session-management-v1-protocol.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <random>

namespace atrium {

namespace {

// A session nobody has asked for in this long is forgotten.
constexpr int64_t kKeepSeconds = 182LL * 24 * 3600;
// Changes are written once the window has held still this long.
constexpr int kSaveDelayMs = 800;

std::string new_session_id() {
    std::random_device rd;
    char out[33];
    for (int i = 0; i < 4; ++i)
        std::snprintf(out + i * 8, 9, "%08x", rd());
    return out;
}

} // namespace

SessionManagement::SessionManagement(Server& server) : server_(server) {
    const int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    server.registry->drop_sessions_before(now - kKeepSeconds);
    global_ = wl_global_create(server.display, &xdg_session_manager_v1_interface, 1, this, &bind);
    save_timer_ = wl_event_loop_add_timer(server.loop, [](void* data) {
        auto* self = static_cast<SessionManagement*>(data);
        for (const View* v : std::exchange(self->dirty_, {}))
            if (const ToplevelSession* t = self->find(v))
                self->save(*t);
        return 0;
    }, this);
}

SessionManagement::~SessionManagement() {
    // What moved since the last write, then cut every resource loose.
    for (const View* v : dirty_)
        if (const ToplevelSession* t = find(v))
            save(*t);
    if (save_timer_)
        wl_event_source_remove(save_timer_);
    for (wl_resource* m : managers_) {
        wl_resource_set_user_data(m, nullptr);
        wl_resource_set_destructor(m, nullptr);
    }
    for (Session* s : sessions_) {
        for (ToplevelSession* t : s->toplevels) {
            wl_resource_set_user_data(t->resource, nullptr);
            wl_resource_set_destructor(t->resource, nullptr);
            delete t;
        }
        wl_resource_set_user_data(s->resource, nullptr);
        wl_resource_set_destructor(s->resource, nullptr);
        delete s;
    }
    if (global_)
        wl_global_destroy(global_);
}

// --- manager ---------------------------------------------------------------------------

void SessionManagement::bind(wl_client* client, void* data, uint32_t version, uint32_t id) {
    auto* self = static_cast<SessionManagement*>(data);
    wl_resource* r = wl_resource_create(client, &xdg_session_manager_v1_interface, int(version), id);
    if (!r) {
        wl_client_post_no_memory(client);
        return;
    }
    static const struct xdg_session_manager_v1_interface impl = {
        .destroy = &destroy_resource,
        .get_session = &get_session,
    };
    wl_resource_set_implementation(r, &impl, self, &manager_gone);
    self->managers_.push_back(r);
}

void SessionManagement::destroy_resource(wl_client*, wl_resource* resource) {
    wl_resource_destroy(resource);
}

void SessionManagement::manager_gone(wl_resource* resource) {
    if (auto* self = static_cast<SessionManagement*>(wl_resource_get_user_data(resource)))
        std::erase(self->managers_, resource);
}

void SessionManagement::get_session(wl_client* client, wl_resource* manager, uint32_t id, uint32_t reason,
                                    const char* session_id) {
    auto* self = static_cast<SessionManagement*>(wl_resource_get_user_data(manager));
    if (reason < XDG_SESSION_MANAGER_V1_REASON_LAUNCH || reason > XDG_SESSION_MANAGER_V1_REASON_SESSION_RESTORE) {
        wl_resource_post_error(manager, XDG_SESSION_MANAGER_V1_ERROR_INVALID_REASON, "unknown reason %u", reason);
        return;
    }
    wl_resource* r = wl_resource_create(client, &xdg_session_v1_interface, wl_resource_get_version(manager), id);
    if (!r) {
        wl_client_post_no_memory(client);
        return;
    }
    static const struct xdg_session_v1_interface impl = {
        .destroy = &destroy_resource,
        .remove = &remove_session,
        .add_toplevel = &add_toplevel,
        .restore_toplevel = &restore_toplevel,
        .remove_toplevel = &remove_toplevel,
    };
    if (!self) {
        wl_resource_set_implementation(r, &impl, nullptr, nullptr);
        return;
    }

    const bool known = session_id && self->server_.registry->has_session(session_id);
    if (known) {
        for (Session* s : self->sessions_)
            if (s->id == session_id) {
                if (s->client == client) {
                    wl_resource_destroy(r);
                    wl_resource_post_error(manager, XDG_SESSION_MANAGER_V1_ERROR_IN_USE,
                                           "session %s is already in use", session_id);
                    return;
                }
                // Another client takes it over.
                xdg_session_v1_send_replaced(s->resource);
                self->make_inert(s);
                break;
            }
    }
    auto* s = new Session{self, r, client, known ? std::string(session_id) : new_session_id()};
    wl_resource_set_implementation(r, &impl, s, &session_gone);
    self->sessions_.push_back(s);
    self->server_.registry->touch_session(s->id, "");
    if (known)
        xdg_session_v1_send_restored(r);
    else
        xdg_session_v1_send_created(r, s->id.c_str());
}

// --- session -----------------------------------------------------------------------------

void SessionManagement::make_inert(Session* s) {
    for (ToplevelSession* t : s->toplevels) {
        t->session = nullptr;
        t->toplevel_destroy.disconnect();
    }
    s->toplevels.clear();
    wl_resource_set_user_data(s->resource, nullptr);
    std::erase(sessions_, s);
    delete s;
}

void SessionManagement::session_gone(wl_resource* resource) {
    // Destroyed: what was saved stays for next time; nothing more is saved.
    if (auto* s = static_cast<Session*>(wl_resource_get_user_data(resource))) {
        for (ToplevelSession* t : s->toplevels)
            if (View* v = s->owner->view_of(*t); v && v->mapped)
                s->owner->save(*t);
        s->owner->make_inert(s);
    }
}

void SessionManagement::remove_session(wl_client*, wl_resource* resource) {
    if (auto* s = static_cast<Session*>(wl_resource_get_user_data(resource))) {
        SessionManagement* self = s->owner;
        const std::string id = s->id;
        self->make_inert(s);
        self->server_.registry->remove_session(id);
    }
    wl_resource_destroy(resource);
}

SessionManagement::ToplevelSession* SessionManagement::track(Session* s, uint32_t id, wl_resource* toplevel_resource,
                                                             const char* name, bool restore) {
    wl_resource* session_resource = s->resource;
    wlr_xdg_toplevel* toplevel = wlr_xdg_toplevel_from_resource(toplevel_resource);
    for (ToplevelSession* t : s->toplevels) {
        if (t->name == name) {
            wl_resource_post_error(session_resource, XDG_SESSION_V1_ERROR_NAME_IN_USE, "name %s is in use", name);
            return nullptr;
        }
        if (t->toplevel == toplevel) {
            wl_resource_post_error(session_resource, XDG_SESSION_V1_ERROR_ALREADY_ADDED, "toplevel already added");
            return nullptr;
        }
    }
    if (restore && toplevel && toplevel->base->initialized) {
        wl_resource_post_error(session_resource, XDG_SESSION_V1_ERROR_ALREADY_MAPPED,
                               "restore_toplevel after the toplevel's first commit");
        return nullptr;
    }
    wl_resource* r = wl_resource_create(wl_resource_get_client(session_resource), &xdg_toplevel_session_v1_interface,
                                        wl_resource_get_version(session_resource), id);
    if (!r) {
        wl_client_post_no_memory(wl_resource_get_client(session_resource));
        return nullptr;
    }
    static const struct xdg_toplevel_session_v1_interface impl = {
        .destroy = &destroy_resource,
        .rename = &rename,
    };
    auto* t = new ToplevelSession{s, r, toplevel, name};
    wl_resource_set_implementation(r, &impl, t, &toplevel_session_gone);
    if (toplevel)
        t->toplevel_destroy.connect(&toplevel->events.destroy, [t](void*) {
            t->toplevel = nullptr;
            t->toplevel_destroy.disconnect();
        });
    s->toplevels.push_back(t);
    if (toplevel && toplevel->app_id)
        s->owner->server_.registry->touch_session(s->id, toplevel->app_id);
    if (restore)
        if (auto w = s->owner->server_.registry->session_window(s->id, name)) {
            t->restore = *w;
            // Before the first configure, which the first commit brings.
            xdg_toplevel_session_v1_send_restored(r);
        }
    return t;
}

void SessionManagement::add_toplevel(wl_client*, wl_resource* session, uint32_t id, wl_resource* toplevel,
                                     const char* name) {
    if (auto* s = static_cast<Session*>(wl_resource_get_user_data(session)))
        s->owner->track(s, id, toplevel, name, false);
    else
        wl_resource_set_implementation(
            wl_resource_create(wl_resource_get_client(session), &xdg_toplevel_session_v1_interface, 1, id), nullptr,
            nullptr, nullptr);
}

void SessionManagement::restore_toplevel(wl_client*, wl_resource* session, uint32_t id, wl_resource* toplevel,
                                         const char* name) {
    if (auto* s = static_cast<Session*>(wl_resource_get_user_data(session)))
        s->owner->track(s, id, toplevel, name, true);
    else
        wl_resource_set_implementation(
            wl_resource_create(wl_resource_get_client(session), &xdg_toplevel_session_v1_interface, 1, id), nullptr,
            nullptr, nullptr);
}

void SessionManagement::remove_toplevel(wl_client*, wl_resource* session, const char* name) {
    auto* s = static_cast<Session*>(wl_resource_get_user_data(session));
    if (!s)
        return;
    for (ToplevelSession* t : s->toplevels)
        if (t->name == name) {
            t->session = nullptr;
            t->toplevel_destroy.disconnect();
            std::erase(s->toplevels, t);
            break;
        }
    s->owner->server_.registry->remove_session_window(s->id, name);
}

// --- toplevel session --------------------------------------------------------------------

void SessionManagement::toplevel_session_gone(wl_resource* resource) {
    auto* t = static_cast<ToplevelSession*>(wl_resource_get_user_data(resource));
    if (!t)
        return;
    if (t->session)
        std::erase(t->session->toplevels, t);
    delete t;
}

void SessionManagement::rename(wl_client*, wl_resource* resource, const char* name) {
    auto* t = static_cast<ToplevelSession*>(wl_resource_get_user_data(resource));
    if (!t || !t->session)
        return;
    for (ToplevelSession* other : t->session->toplevels)
        if (other != t && other->name == name) {
            wl_resource_post_error(t->session->resource, XDG_SESSION_V1_ERROR_NAME_IN_USE, "name %s is in use", name);
            return;
        }
    t->session->owner->server_.registry->rename_session_window(t->session->id, t->name, name);
    t->name = name;
}

// --- windows -------------------------------------------------------------------------------

View* SessionManagement::view_of(const ToplevelSession& t) const {
    if (!t.toplevel)
        return nullptr;
    for (View* v : server_.views)
        if (v->kind == View::Kind::Xdg && static_cast<XdgView*>(v)->toplevel == t.toplevel)
            return v;
    return nullptr;
}

SessionManagement::ToplevelSession* SessionManagement::find(const View* view) const {
    if (!view || view->kind != View::Kind::Xdg)
        return nullptr;
    const wlr_xdg_toplevel* toplevel = static_cast<const XdgView*>(view)->toplevel;
    for (Session* s : sessions_)
        for (ToplevelSession* t : s->toplevels)
            if (t->toplevel == toplevel)
                return t;
    return nullptr;
}

const SessionWindow* SessionManagement::restoring(const View* view) const {
    const ToplevelSession* t = find(view);
    return t && t->restore ? &*t->restore : nullptr;
}

void SessionManagement::save(const ToplevelSession& t) {
    View* v = view_of(t);
    if (!t.session || !v || !v->mapped || !v->output)
        return;
    server_.registry->put_session_window(t.session->id,
                                         SessionWindow{t.name, server_.placement_of(v), v->fullscreen});
}

void SessionManagement::view_changed(const View* view) {
    if (!find(view))
        return;
    if (std::ranges::find(dirty_, view) == dirty_.end())
        dirty_.push_back(view);
    wl_event_source_timer_update(save_timer_, kSaveDelayMs);
}

void SessionManagement::view_unmapping(const View* view) {
    std::erase(dirty_, view);
    if (ToplevelSession* t = find(view)) {
        save(*t);
        t->restore.reset();  // it has been placed; mapped again, it goes where it was last
    }
}

} // namespace atrium
