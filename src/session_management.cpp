#include "session_management.hpp"

#include "server.hpp"
#include "view.hpp"

#include "xdg-session-management-v1-server.hpp"

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

using wl::XdgSessionManagerV1;
using wl::XdgSessionV1;
using wl::XdgToplevelSessionV1;

SessionManagement::SessionManagement(Server& server) : server_(server) {
    const int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    server.registry->drop_sessions_before(now - kKeepSeconds);
    global_ = wl::Global::create<XdgSessionManagerV1>(server.display, 1,
        [this](wl_client* client, uint32_t version, uint32_t id) {
            auto* m = wl::make<XdgSessionManagerV1>(client, version, id);
            if (!m)
                return;
            m->on_get_session([this](XdgSessionManagerV1* self, uint32_t id, uint32_t reason, const char* session_id) {
                get_session(self, id, reason, session_id);
            });
            std::erase_if(managers_, [](const auto& w) { return !w; });
            managers_.push_back(m);
        });
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
    global_.reset();
    for (auto& m : managers_)
        if (m)
            m->detach();
    for (ToplevelSession* t : toplevels_) {
        if (t->resource)
            t->resource->detach();
        delete t;
    }
    for (Session* s : sessions_) {
        if (s->resource)
            s->resource->detach();
        delete s;
    }
}

// --- manager ---------------------------------------------------------------------------

void SessionManagement::get_session(XdgSessionManagerV1* manager, uint32_t id, uint32_t reason,
                                    const char* session_id) {
    using Reason = XdgSessionManagerV1::Reason;
    if (reason < uint32_t(Reason::Launch) || reason > uint32_t(Reason::SessionRestore)) {
        manager->post_error(uint32_t(XdgSessionManagerV1::Error::InvalidReason), "unknown reason");
        return;
    }
    wl_client* client = manager->client();
    const bool known = session_id && server_.registry->has_session(session_id);
    if (known) {
        for (Session* s : sessions_)
            if (s->id == session_id) {
                if (s->client == client) {
                    manager->post_error(uint32_t(XdgSessionManagerV1::Error::InUse), "the session is already in use");
                    return;
                }
                // Another client takes it over.
                if (s->resource)
                    s->resource->send_replaced();
                make_inert(s);
                break;
            }
    }
    auto* r = wl::make<XdgSessionV1>(client, manager->version(), id);
    if (!r)
        return;
    auto* s = new Session{r, client, known ? std::string(session_id) : new_session_id()};
    r->on_remove([this, s](XdgSessionV1*) {
        const std::string sid = s->id;
        make_inert(s);
        server_.registry->remove_session(sid);
    });
    r->on_add_toplevel([this, s](XdgSessionV1* self, uint32_t id, wl_resource* toplevel, const char* name) {
        track(s, self, id, toplevel, name, false);
    });
    r->on_restore_toplevel([this, s](XdgSessionV1* self, uint32_t id, wl_resource* toplevel, const char* name) {
        track(s, self, id, toplevel, name, true);
    });
    r->on_remove_toplevel([this, s](XdgSessionV1*, const char* name) { remove_toplevel(s, name); });
    r->on_gone([this, s] { session_gone(s); });
    sessions_.push_back(s);
    server_.registry->touch_session(s->id, "");
    if (known)
        r->send_restored();
    else
        r->send_created(s->id.c_str());
}

// --- session -----------------------------------------------------------------------------

void SessionManagement::make_inert(Session* s) {
    for (ToplevelSession* t : s->toplevels) {
        t->session = nullptr;
        t->toplevel_destroy.disconnect();
    }
    s->toplevels.clear();
    if (s->resource)
        s->resource->detach();
    std::erase(sessions_, s);
    delete s;
}

void SessionManagement::session_gone(Session* s) {
    // Destroyed: what was saved stays for next time; nothing more is saved.
    for (ToplevelSession* t : s->toplevels)
        if (View* v = view_of(*t); v && v->mapped)
            save(*t);
    make_inert(s);
}

void SessionManagement::track(Session* s, XdgSessionV1* session, uint32_t id, wl_resource* toplevel_resource,
                              const char* name, bool restore) {
    wl::Toplevel* toplevel = wl::Toplevel::from(toplevel_resource);
    for (ToplevelSession* t : s->toplevels) {
        if (t->name == name) {
            session->post_error(uint32_t(XdgSessionV1::Error::NameInUse), "the name is in use");
            return;
        }
        if (t->toplevel == toplevel) {
            session->post_error(uint32_t(XdgSessionV1::Error::AlreadyAdded), "toplevel already added");
            return;
        }
    }
    if (restore && toplevel && toplevel->base() && toplevel->base()->initialized()) {
        session->post_error(uint32_t(XdgSessionV1::Error::AlreadyMapped),
                            "restore_toplevel after the toplevel's first commit");
        return;
    }
    auto* r = wl::make<XdgToplevelSessionV1>(session->client(), session->version(), id);
    if (!r)
        return;
    auto* t = new ToplevelSession{s, r, toplevel, name};
    r->on_rename([this, t](XdgToplevelSessionV1*, const char* name) { rename(t, name); });
    r->on_gone([this, t] {
        if (t->session)
            std::erase(t->session->toplevels, t);
        std::erase(toplevels_, t);
        delete t;
    });
    toplevels_.push_back(t);
    if (toplevel)
        t->toplevel_destroy = toplevel->events.destroy.connect([t] {
            t->toplevel = nullptr;
            t->toplevel_destroy.disconnect();
        });
    s->toplevels.push_back(t);
    if (toplevel && !toplevel->app_id().empty())
        server_.registry->touch_session(s->id, toplevel->app_id());
    if (restore)
        if (auto w = server_.registry->session_window(s->id, name)) {
            t->restore = *w;
            // Before the first configure, which the first commit brings.
            r->send_restored();
        }
}

void SessionManagement::remove_toplevel(Session* s, const char* name) {
    for (ToplevelSession* t : s->toplevels)
        if (t->name == name) {
            t->session = nullptr;
            t->toplevel_destroy.disconnect();
            std::erase(s->toplevels, t);
            break;
        }
    server_.registry->remove_session_window(s->id, name);
}

// --- toplevel session --------------------------------------------------------------------

void SessionManagement::rename(ToplevelSession* t, const char* name) {
    if (!t->session)
        return;
    for (ToplevelSession* other : t->session->toplevels)
        if (other != t && other->name == name) {
            if (t->session->resource)
                t->session->resource->post_error(uint32_t(XdgSessionV1::Error::NameInUse), "the name is in use");
            return;
        }
    server_.registry->rename_session_window(t->session->id, t->name, name);
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
    const wl::Toplevel* toplevel = static_cast<const XdgView*>(view)->toplevel;
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
