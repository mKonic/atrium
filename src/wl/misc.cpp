#include "wl/misc.hpp"

#include "hyprland-global-shortcuts-v1-server.hpp"
#include "security-context-v1-server.hpp"
#include "xdg-foreign-unstable-v2-server.hpp"

#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <random>

namespace atrium::wl {

namespace {

template <class List>
void detach_all(List& list) {
    for (auto& w : list)
        if (w)
            w->detach();
}

std::string random_handle() {
    std::random_device rd;
    char out[33];
    for (int i = 0; i < 4; ++i)
        std::snprintf(out + i * 8, 9, "%08x", rd());
    return out;
}

} // namespace

// ---- xdg-foreign -------------------------------------------------------------------

struct XdgForeign::Exported {
    std::string handle;
    Toplevel* toplevel;
    Weak<Resource> resource;
    std::vector<Weak<Resource>> imports;  // imported objects to tell when it goes
    Connection toplevel_gone;
};

XdgForeign::XdgForeign(wl_display* display) {
    exporter_ = Global::create<ZxdgExporterV2>(display, 1, [this](wl_client* client, uint32_t version, uint32_t id) {
        auto* m = make<ZxdgExporterV2>(client, version, id);
        if (!m)
            return;
        m->on_export_toplevel([this](ZxdgExporterV2* self, uint32_t id, wl_resource* surface_res) {
            auto* r = make<ZxdgExportedV2>(self->client(), self->version(), id);
            Toplevel* t = Toplevel::from(Surface::from(surface_res));
            if (!r)
                return;
            if (!t) {
                self->post_error(uint32_t(ZxdgExporterV2::Error::InvalidSurface), "only toplevels can be exported");
                return;
            }
            auto e = std::make_unique<Exported>(Exported{random_handle(), t});
            Exported* ep = e.get();
            ep->resource = r;
            exported_.push_back(std::move(e));
            auto drop = [this, ep] {
                for (auto& w : ep->imports)
                    if (auto* im = static_cast<ZxdgImportedV2*>(w.get()); im && !im->inert()) {
                        im->send_destroyed();
                        im->detach();
                    }
                if (Resource* res = ep->resource.get())
                    res->detach();
                std::erase_if(exported_, [ep](const auto& x) { return x.get() == ep; });
            };
            r->on_gone(drop);
            ep->toplevel_gone = t->events.destroy.connect(drop);
            r->send_handle(ep->handle.c_str());
        });
        std::erase_if(managers_, [](const auto& w) { return !w; });
        managers_.push_back(m);
    });
    importer_ = Global::create<ZxdgImporterV2>(display, 1, [this](wl_client* client, uint32_t version, uint32_t id) {
        auto* m = make<ZxdgImporterV2>(client, version, id);
        if (!m)
            return;
        m->on_import_toplevel([this](ZxdgImporterV2* self, uint32_t id, const char* handle) {
            auto* r = make<ZxdgImportedV2>(self->client(), self->version(), id);
            if (!r)
                return;
            auto it = std::ranges::find_if(exported_, [handle](const auto& e) { return e->handle == handle; });
            if (it == exported_.end()) {
                r->send_destroyed();  // no such window (any more)
                r->detach();
                return;
            }
            Exported* ep = it->get();
            ep->imports.push_back(r);
            r->on_set_parent_of([this, ep](ZxdgImportedV2* self, wl_resource* surface_res) {
                Toplevel* child = Toplevel::from(Surface::from(surface_res));
                if (!child) {
                    self->post_error(uint32_t(ZxdgImportedV2::Error::InvalidSurface), "only toplevels have parents");
                    return;
                }
                set_parent.emit(child, ep->toplevel);
            });
        });
        std::erase_if(managers_, [](const auto& w) { return !w; });
        managers_.push_back(m);
    });
}

XdgForeign::~XdgForeign() {
    exporter_.reset();
    importer_.reset();
    detach_all(managers_);
    for (auto& e : exported_) {
        if (Resource* r = e->resource.get())
            r->detach();
        detach_all(e->imports);
    }
}

// ---- global shortcuts -------------------------------------------------------------------

GlobalShortcuts::GlobalShortcuts(wl_display* display) {
    global_ = Global::create<HyprlandGlobalShortcutsManagerV1>(display, 1, [this](wl_client* client,
                                                                                  uint32_t version, uint32_t id) {
        auto* m = make<HyprlandGlobalShortcutsManagerV1>(client, version, id);
        if (!m)
            return;
        m->on_register_shortcut([this](HyprlandGlobalShortcutsManagerV1* self, uint32_t id, const char* sid,
                                       const char* app_id, const char* description, const char* trigger) {
            auto* r = make<HyprlandGlobalShortcutV1>(self->client(), self->version(), id);
            if (!r)
                return;
            if (std::ranges::any_of(shortcuts_, [&](const auto& s) { return s->id == sid && s->app_id == app_id; })) {
                self->post_error(uint32_t(HyprlandGlobalShortcutsManagerV1::Error::AlreadyTaken),
                                 "the app already has a shortcut with that id");
                return;
            }
            auto s = std::make_unique<Shortcut>(Shortcut{sid, app_id, description, trigger, self->client()});
            Shortcut* sp = s.get();
            sp->resource = r;
            shortcuts_.push_back(std::move(s));
            r->on_gone([this, sp] {
                unregistered.emit(sp);
                std::erase_if(shortcuts_, [sp](const auto& x) { return x.get() == sp; });
            });
            registered.emit(sp);
        });
        std::erase_if(managers_, [](const auto& w) { return !w; });
        managers_.push_back(m);
    });
}

GlobalShortcuts::~GlobalShortcuts() {
    global_.reset();
    detach_all(managers_);
    for (auto& s : shortcuts_)
        if (Resource* r = s->resource.get())
            r->detach();
}

void GlobalShortcuts::press(Shortcut* s, uint64_t time_ns) {
    if (auto* r = static_cast<HyprlandGlobalShortcutV1*>(s->resource.get())) {
        const uint64_t sec = time_ns / 1000000000;
        r->send_pressed(uint32_t(sec >> 32), uint32_t(sec), uint32_t(time_ns % 1000000000));
    }
}

void GlobalShortcuts::release(Shortcut* s, uint64_t time_ns) {
    if (auto* r = static_cast<HyprlandGlobalShortcutV1*>(s->resource.get())) {
        const uint64_t sec = time_ns / 1000000000;
        r->send_released(uint32_t(sec >> 32), uint32_t(sec), uint32_t(time_ns % 1000000000));
    }
}

// ---- security context -------------------------------------------------------------------

// A committed context: accepts on the sandbox's socket until it closes the
// close fd; each client it accepts carries the metadata.
struct SecurityContexts::Listener {
    SecurityContexts* owner;
    int listen_fd, close_fd;
    Metadata metadata;
    wl_event_source *listen_source = nullptr, *close_source = nullptr;

    ~Listener() {
        if (listen_source)
            wl_event_source_remove(listen_source);
        if (close_source)
            wl_event_source_remove(close_source);
        close(listen_fd);
        close(close_fd);
    }
};

namespace {

struct ClientGone {
    wl_listener listener;
    SecurityContexts* owner;
    std::map<const wl_client*, SecurityContexts::Metadata>* clients;
};

} // namespace

SecurityContexts::SecurityContexts(wl_display* display) : display_(display) {
    global_ = Global::create<WpSecurityContextManagerV1>(display, 1, [this](wl_client* client, uint32_t version,
                                                                            uint32_t id) {
        auto* m = make<WpSecurityContextManagerV1>(client, version, id);
        if (!m)
            return;
        m->on_create_listener([this](WpSecurityContextManagerV1* self, uint32_t id, int listen_fd, int close_fd) {
            auto* r = make<WpSecurityContextV1>(self->client(), self->version(), id);
            auto close_both = [&] {
                close(listen_fd);
                close(close_fd);
            };
            if (!r) {
                close_both();
                return;
            }
            // A sandboxed client can't make a context of its own.
            if (lookup(self->client())) {
                close_both();
                self->post_error(uint32_t(WpSecurityContextManagerV1::Error::Nested), "a nested security context");
                return;
            }
            int accepting = 0;
            socklen_t len = sizeof accepting;
            if (getsockopt(listen_fd, SOL_SOCKET, SO_ACCEPTCONN, &accepting, &len) != 0 || !accepting) {
                close_both();
                self->post_error(uint32_t(WpSecurityContextManagerV1::Error::InvalidListenFd),
                                 "not a listening socket");
                return;
            }
            // Made in place: a Listener closes its fds when it goes.
            auto l = std::make_shared<std::unique_ptr<Listener>>(
                std::unique_ptr<Listener>(new Listener{this, listen_fd, close_fd, {}}));
            auto used = std::make_shared<bool>(false);
            auto set_once = [used](WpSecurityContextV1* self, std::string& field, const char* value) {
                if (*used) {
                    self->post_error(uint32_t(WpSecurityContextV1::Error::AlreadyUsed), "already committed");
                    return;
                }
                if (!field.empty()) {
                    self->post_error(uint32_t(WpSecurityContextV1::Error::AlreadySet), "already set");
                    return;
                }
                field = value;
            };
            r->on_set_sandbox_engine([l, set_once](WpSecurityContextV1* self, const char* name) {
                if (*l)
                    set_once(self, (*l)->metadata.sandbox_engine, name);
            });
            r->on_set_app_id([l, set_once](WpSecurityContextV1* self, const char* app_id) {
                if (*l)
                    set_once(self, (*l)->metadata.app_id, app_id);
            });
            r->on_set_instance_id([l, set_once](WpSecurityContextV1* self, const char* instance_id) {
                if (*l)
                    set_once(self, (*l)->metadata.instance_id, instance_id);
            });
            r->on_commit([this, l, used](WpSecurityContextV1* self) {
                if (std::exchange(*used, true) || !*l) {
                    self->post_error(uint32_t(WpSecurityContextV1::Error::AlreadyUsed), "already committed");
                    return;
                }
                Listener* lp = l->get();
                wl_event_loop* loop = wl_display_get_event_loop(display_);
                lp->listen_source = wl_event_loop_add_fd(loop, lp->listen_fd, WL_EVENT_READABLE,
                                                         [](int fd, uint32_t, void* data) {
                    auto* lp = static_cast<Listener*>(data);
                    const int client_fd = accept4(fd, nullptr, nullptr, SOCK_CLOEXEC);
                    if (client_fd < 0)
                        return 0;
                    wl_client* c = wl_client_create(lp->owner->display_, client_fd);
                    if (!c)
                        return 0;
                    lp->owner->clients_[c] = lp->metadata;
                    auto* gone = new ClientGone{{}, lp->owner, &lp->owner->clients_};
                    gone->listener.notify = [](wl_listener* l, void* data) {
                        ClientGone* g = wl_container_of(l, g, listener);
                        g->clients->erase(static_cast<wl_client*>(data));
                        wl_list_remove(&g->listener.link);
                        delete g;
                    };
                    wl_client_add_destroy_listener(c, &gone->listener);
                    return 0;
                }, lp);
                // Closed (or hung up) by the sandbox: no more clients.
                lp->close_source = wl_event_loop_add_fd(loop, lp->close_fd, 0, [](int, uint32_t, void* data) {
                    auto* lp = static_cast<Listener*>(data);
                    SecurityContexts* owner = lp->owner;
                    std::erase_if(owner->listeners_, [lp](const auto& x) { return x.get() == lp; });
                    return 0;
                }, lp);
                listeners_.push_back(std::move(*l));
            });
        });
        std::erase_if(managers_, [](const auto& w) { return !w; });
        managers_.push_back(m);
    });
}

SecurityContexts::~SecurityContexts() {
    global_.reset();
    detach_all(managers_);
    listeners_.clear();
}

const SecurityContexts::Metadata* SecurityContexts::lookup(const wl_client* client) const {
    auto it = clients_.find(client);
    return it == clients_.end() ? nullptr : &it->second;
}

} // namespace atrium::wl
