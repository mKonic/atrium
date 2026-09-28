#include "wl/desktop.hpp"

#include "wl/output.hpp"
#include "wl/seat.hpp"

#include "ext-foreign-toplevel-list-v1-server.hpp"
#include "ext-workspace-v1-server.hpp"
#include "wlr-foreign-toplevel-management-unstable-v1-server.hpp"
#include "wlr-gamma-control-unstable-v1-server.hpp"
#include "wlr-output-management-unstable-v1-server.hpp"
#include "wlr-output-power-management-unstable-v1-server.hpp"

#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <random>

namespace atrium::wl {

namespace {

template <class List>
void detach_all(List& list) {
    for (auto& w : list)
        if (w)
            w->detach();
}

wl_array as_array(std::vector<uint32_t>& v) {
    return wl_array{v.size() * sizeof(uint32_t), v.size() * sizeof(uint32_t), v.data()};
}

} // namespace

// ---- foreign toplevels --------------------------------------------------------------

ForeignToplevels::ForeignToplevels(wl_display* display) {
    ext_global_ = Global::create<ExtForeignToplevelListV1>(display, 1, [this](wl_client* client, uint32_t version,
                                                                              uint32_t id) {
        auto* l = make<ExtForeignToplevelListV1>(client, version, id);
        if (!l)
            return;
        l->on_stop([](ExtForeignToplevelListV1* self) {
            self->send_finished();
            self->detach();  // no more toplevels for it
        });
        std::erase_if(ext_lists_, [](const auto& w) { return !w; });
        ext_lists_.push_back(l);
        for (auto& h : handles_)
            announce_ext(l, h.get());
    });
    wlr_global_ = Global::create<ZwlrForeignToplevelManagerV1>(display, 3, [this](wl_client* client, uint32_t version,
                                                                                  uint32_t id) {
        auto* m = make<ZwlrForeignToplevelManagerV1>(client, version, id);
        if (!m)
            return;
        m->on_stop([](ZwlrForeignToplevelManagerV1* self) {
            self->send_finished();
            self->detach();
        });
        std::erase_if(wlr_managers_, [](const auto& w) { return !w; });
        wlr_managers_.push_back(m);
        for (auto& h : handles_)
            announce_wlr(m, h.get());
    });
}

ForeignToplevels::~ForeignToplevels() {
    ext_global_.reset();
    wlr_global_.reset();
    detach_all(ext_lists_);
    detach_all(wlr_managers_);
    for (auto& h : handles_) {
        detach_all(h->ext_);
        detach_all(h->wlr_);
    }
}

void ForeignToplevels::announce_ext(Resource* list_res, Handle* h) {
    auto* list = static_cast<ExtForeignToplevelListV1*>(list_res);
    if (list->inert())
        return;
    auto* r = make<ExtForeignToplevelHandleV1>(list->client(), list->version(), 0);
    if (!r)
        return;
    list->send_toplevel(r);
    r->send_identifier(h->identifier_.c_str());
    r->send_title(h->info_.title.c_str());
    r->send_app_id(h->info_.app_id.c_str());
    r->send_done();
    std::erase_if(h->ext_, [](const auto& w) { return !w; });
    h->ext_.push_back(r);
}

void ForeignToplevels::announce_wlr(Resource* manager_res, Handle* h) {
    auto* m = static_cast<ZwlrForeignToplevelManagerV1*>(manager_res);
    if (m->inert())
        return;
    auto* r = make<ZwlrForeignToplevelHandleV1>(m->client(), m->version(), 0);
    if (!r)
        return;
    r->on_set_maximized([h](ZwlrForeignToplevelHandleV1*) { h->events.request_maximize.emit(true); });
    r->on_unset_maximized([h](ZwlrForeignToplevelHandleV1*) { h->events.request_maximize.emit(false); });
    r->on_set_minimized([h](ZwlrForeignToplevelHandleV1*) { h->events.request_minimize.emit(true); });
    r->on_unset_minimized([h](ZwlrForeignToplevelHandleV1*) { h->events.request_minimize.emit(false); });
    r->on_activate([h](ZwlrForeignToplevelHandleV1*, wl_resource* seat) {
        h->events.request_activate.emit(Seat::from(seat));
    });
    r->on_close([h](ZwlrForeignToplevelHandleV1*) { h->events.request_close.emit(); });
    r->on_set_rectangle([h](ZwlrForeignToplevelHandleV1* self, wl_resource* surface, int32_t x, int32_t y, int32_t w,
                            int32_t hh) {
        if (w < 0 || hh < 0) {
            self->post_error(uint32_t(ZwlrForeignToplevelHandleV1::Error::InvalidRectangle), "a negative size");
            return;
        }
        if (Surface* s = Surface::from(surface))
            h->events.set_rectangle.emit(s, Box{x, y, w, hh});
    });
    r->on_set_fullscreen([h](ZwlrForeignToplevelHandleV1*, wl_resource* output) {
        h->events.request_fullscreen.emit(true, output ? Output::from(output) : nullptr);
    });
    r->on_unset_fullscreen([h](ZwlrForeignToplevelHandleV1*) { h->events.request_fullscreen.emit(false, nullptr); });
    m->send_toplevel(r);
    std::erase_if(h->wlr_, [](const auto& w) { return !w; });
    h->wlr_.push_back(r);
    send_wlr(r, *h, nullptr);
}

// Sends what differs from `old` (everything when null), then done.
void ForeignToplevels::send_wlr(Resource* res, const Handle& h, const Info* old) {
    auto* r = static_cast<ZwlrForeignToplevelHandleV1*>(res);
    const Info& i = h.info_;
    if (!old || old->title != i.title)
        r->send_title(i.title.c_str());
    if (!old || old->app_id != i.app_id)
        r->send_app_id(i.app_id.c_str());
    for (Output* o : i.outputs)
        if (!old || std::ranges::find(old->outputs, o) == old->outputs.end())
            for (WlOutput* wo : o->resources_for(r->client()))
                r->send_output_enter(wo->resource());
    if (old)
        for (Output* o : old->outputs)
            if (std::ranges::find(i.outputs, o) == i.outputs.end())
                for (WlOutput* wo : o->resources_for(r->client()))
                    r->send_output_leave(wo->resource());
    std::vector<uint32_t> states;
    if (i.maximized)
        states.push_back(0);
    if (i.minimized)
        states.push_back(1);
    if (i.activated)
        states.push_back(2);
    if (i.fullscreen && r->version() >= 2)
        states.push_back(3);
    wl_array a = as_array(states);
    r->send_state(&a);
    if (!old && r->version() >= 3 && h.parent_) {
        for (auto& pw : h.parent_->wlr_)
            if (pw && pw->client() == r->client())
                r->send_parent(static_cast<ZwlrForeignToplevelHandleV1*>(pw.get()));
    }
    r->send_done();
}

ForeignToplevels::Handle* ForeignToplevels::create(const Info& info) {
    static const uint64_t salt = std::random_device{}() | (uint64_t(std::random_device{}()) << 32);
    auto h = std::make_unique<Handle>();
    h->owner_ = this;
    h->info_ = info;
    char id[40];
    std::snprintf(id, sizeof id, "%016llx%08llx", static_cast<unsigned long long>(salt),
                  static_cast<unsigned long long>(next_id_++));
    h->identifier_ = id;
    Handle* hp = h.get();
    handles_.push_back(std::move(h));
    for (auto& l : ext_lists_)
        if (l)
            announce_ext(l.get(), hp);
    for (auto& m : wlr_managers_)
        if (m)
            announce_wlr(m.get(), hp);
    return hp;
}

void ForeignToplevels::destroy(Handle* h) {
    for (auto& c : handles_)
        if (c->parent_ == h)
            c->set_parent(nullptr);
    for (auto& w : h->ext_)
        if (auto* r = static_cast<ExtForeignToplevelHandleV1*>(w.get())) {
            r->send_closed();
            r->detach();
        }
    for (auto& w : h->wlr_)
        if (auto* r = static_cast<ZwlrForeignToplevelHandleV1*>(w.get())) {
            r->send_closed();
            r->detach();
        }
    std::erase_if(handles_, [h](const auto& x) { return x.get() == h; });
}

ForeignToplevels::Handle* ForeignToplevels::from(wl_resource* resource) const {
    for (const auto& h : handles_) {
        for (const auto& w : h->ext_)
            if (w && w->resource() == resource)
                return h.get();
        for (const auto& w : h->wlr_)
            if (w && w->resource() == resource)
                return h.get();
    }
    return nullptr;
}

void ForeignToplevels::Handle::update(const Info& info) {
    if (info == info_)
        return;
    const Info old = std::exchange(info_, info);
    for (auto& w : ext_)
        if (auto* r = static_cast<ExtForeignToplevelHandleV1*>(w.get()); r && !r->inert()) {
            if (old.title == info.title && old.app_id == info.app_id)
                continue;
            if (old.title != info.title)
                r->send_title(info.title.c_str());
            if (old.app_id != info.app_id)
                r->send_app_id(info.app_id.c_str());
            r->send_done();
        }
    for (auto& w : wlr_)
        if (Resource* r = w.get(); r && !r->inert())
            owner_->send_wlr(r, *this, &old);
}

void ForeignToplevels::Handle::set_parent(Handle* parent) {
    if (parent == parent_)
        return;
    parent_ = parent;
    for (auto& w : wlr_)
        if (auto* r = static_cast<ZwlrForeignToplevelHandleV1*>(w.get()); r && !r->inert() && r->version() >= 3) {
            ZwlrForeignToplevelHandleV1* p = nullptr;
            if (parent)
                for (auto& pw : parent->wlr_)
                    if (pw && pw->client() == r->client())
                        p = static_cast<ZwlrForeignToplevelHandleV1*>(pw.get());
            r->send_parent(p);
            r->send_done();
        }
}

// ---- workspaces -------------------------------------------------------------------------

struct Workspaces::Manager {
    Weak<ExtWorkspaceManagerV1> resource;
    std::map<Group*, Weak<ExtWorkspaceGroupHandleV1>> groups;
    std::map<Workspace*, Weak<ExtWorkspaceHandleV1>> workspaces;
    std::vector<Request> pending;
};

Workspaces::Workspaces(wl_display* display) : display_(display) {
    global_ = Global::create<ExtWorkspaceManagerV1>(display, 1, [this](wl_client* client, uint32_t version,
                                                                       uint32_t id) {
        auto* r = make<ExtWorkspaceManagerV1>(client, version, id);
        if (!r)
            return;
        auto m = std::make_unique<Manager>();
        Manager* mp = m.get();
        mp->resource = r;
        managers_.push_back(std::move(m));
        r->on_commit([this, mp](ExtWorkspaceManagerV1*) {
            if (!mp->pending.empty())
                requests.emit(std::exchange(mp->pending, {}));
        });
        r->on_stop([](ExtWorkspaceManagerV1* self) {
            self->send_finished();
            self->detach();
        });
        r->on_gone([this, mp] { std::erase_if(managers_, [mp](const auto& x) { return x.get() == mp; }); });
        for (auto& g : groups_)
            send_group(*mp, g.get());
        for (auto& w : workspaces_)
            send_workspace(*mp, w.get(), true);
        r->send_done();
    });
}

Workspaces::~Workspaces() {
    if (done_idle_)
        wl_event_source_remove(done_idle_);
    global_.reset();
    for (auto& m : managers_) {
        for (auto& [g, w] : m->groups)
            if (w)
                w->detach();
        for (auto& [ws, w] : m->workspaces)
            if (w)
                w->detach();
        if (m->resource)
            m->resource->on_gone(nullptr), m->resource->detach();
    }
}

void Workspaces::schedule_done() {
    if (done_idle_)
        return;
    done_idle_ = wl_event_loop_add_idle(wl_display_get_event_loop(display_), [](void* data) {
        auto* self = static_cast<Workspaces*>(data);
        self->done_idle_ = nullptr;
        for (auto& m : self->managers_)
            if (auto* r = m->resource.get(); r && !r->inert())
                r->send_done();
    }, this);
}

void Workspaces::send_group(Manager& m, Group* g) {
    auto* mr = m.resource.get();
    if (!mr || mr->inert())
        return;
    auto* r = make<ExtWorkspaceGroupHandleV1>(mr->client(), mr->version(), 0);
    if (!r)
        return;
    r->on_create_workspace([&m, g](ExtWorkspaceGroupHandleV1*, const char* name) {
        m.pending.push_back({Request::Kind::Create, nullptr, g, std::string(name)});
    });
    m.groups[g] = r;
    mr->send_workspace_group(r);
    r->send_capabilities(g->capabilities);
    for (Output* o : g->outputs)
        for (WlOutput* wo : o->resources_for(r->client()))
            r->send_output_enter(wo->resource());
}

void Workspaces::send_workspace(Manager& m, Workspace* w, bool fresh) {
    auto* mr = m.resource.get();
    if (!mr || mr->inert())
        return;
    ExtWorkspaceHandleV1* r = nullptr;
    if (fresh) {
        r = make<ExtWorkspaceHandleV1>(mr->client(), mr->version(), 0);
        if (!r)
            return;
        r->on_activate([&m, w](ExtWorkspaceHandleV1*) { m.pending.push_back({Request::Kind::Activate, w, nullptr, {}}); });
        r->on_deactivate([&m, w](ExtWorkspaceHandleV1*) { m.pending.push_back({Request::Kind::Deactivate, w, nullptr, {}}); });
        r->on_remove([&m, w](ExtWorkspaceHandleV1*) { m.pending.push_back({Request::Kind::Remove, w, nullptr, {}}); });
        r->on_assign([&m, w](ExtWorkspaceHandleV1*, ExtWorkspaceGroupHandleV1* group_res) {
            for (auto& [g, gw] : m.groups)
                if (gw.get() == group_res)
                    m.pending.push_back({Request::Kind::Assign, w, g, {}});
        });
        m.workspaces[w] = r;
        mr->send_workspace(r);
        if (!w->id.empty())
            r->send_id(w->id.c_str());
    } else {
        r = m.workspaces[w].get();
        if (!r)
            return;
    }
    r->send_name(w->name.c_str());
    wl_array c = as_array(w->coordinates);
    r->send_coordinates(&c);
    r->send_state(w->state);
    r->send_capabilities(w->capabilities);
    if (fresh && w->group)
        if (auto* g = m.groups[w->group].get())
            g->send_workspace_enter(r);
}

Workspaces::Group* Workspaces::add_group(uint32_t capabilities) {
    auto g = std::make_unique<Group>();
    g->capabilities = capabilities;
    Group* gp = g.get();
    groups_.push_back(std::move(g));
    for (auto& m : managers_)
        send_group(*m, gp);
    schedule_done();
    return gp;
}

void Workspaces::remove_group(Group* g) {
    for (auto& w : workspaces_)
        if (w->group == g)
            w->group = nullptr;
    for (auto& m : managers_)
        if (auto it = m->groups.find(g); it != m->groups.end()) {
            if (auto* r = it->second.get()) {
                r->send_removed();
                r->detach();
            }
            m->groups.erase(it);
        }
    std::erase_if(groups_, [g](const auto& x) { return x.get() == g; });
    schedule_done();
}

void Workspaces::set_group_outputs(Group* g, std::vector<Output*> outputs) {
    for (auto& m : managers_)
        if (auto* r = m->groups[g].get()) {
            for (Output* o : g->outputs)
                if (std::ranges::find(outputs, o) == outputs.end())
                    for (WlOutput* wo : o->resources_for(r->client()))
                        r->send_output_leave(wo->resource());
            for (Output* o : outputs)
                if (std::ranges::find(g->outputs, o) == g->outputs.end())
                    for (WlOutput* wo : o->resources_for(r->client()))
                        r->send_output_enter(wo->resource());
        }
    g->outputs = std::move(outputs);
    schedule_done();
}

Workspaces::Workspace* Workspaces::add_workspace(Group* group, const std::string& id, const std::string& name) {
    auto w = std::make_unique<Workspace>();
    w->id = id;
    w->name = name;
    w->group = group;
    Workspace* wp = w.get();
    workspaces_.push_back(std::move(w));
    for (auto& m : managers_)
        send_workspace(*m, wp, true);
    schedule_done();
    return wp;
}

void Workspaces::remove_workspace(Workspace* w) {
    for (auto& m : managers_) {
        for (auto& p : m->pending)
            if (p.workspace == w)
                p.workspace = nullptr;
        if (auto it = m->workspaces.find(w); it != m->workspaces.end()) {
            if (auto* r = it->second.get()) {
                if (w->group)
                    if (auto* g = m->groups[w->group].get())
                        g->send_workspace_leave(r);
                r->send_removed();
                r->detach();
            }
            m->workspaces.erase(it);
        }
    }
    std::erase_if(workspaces_, [w](const auto& x) { return x.get() == w; });
    schedule_done();
}

void Workspaces::update(Workspace* w, const std::string& name, uint32_t state, std::vector<uint32_t> coordinates,
                        uint32_t capabilities) {
    if (w->name == name && w->state == state && w->coordinates == coordinates && w->capabilities == capabilities)
        return;
    w->name = name;
    w->state = state;
    w->coordinates = std::move(coordinates);
    w->capabilities = capabilities;
    for (auto& m : managers_)
        send_workspace(*m, w, false);
    schedule_done();
}

void Workspaces::move(Workspace* w, Group* group) {
    if (w->group == group)
        return;
    for (auto& m : managers_)
        if (auto* r = m->workspaces[w].get()) {
            if (w->group)
                if (auto* g = m->groups[w->group].get())
                    g->send_workspace_leave(r);
            if (group)
                if (auto* g = m->groups[group].get())
                    g->send_workspace_enter(r);
        }
    w->group = group;
    schedule_done();
}

// ---- output management --------------------------------------------------------------------

struct OutputManagement::Client {
    Weak<ZwlrOutputManagerV1> resource;
    std::vector<Weak<ZwlrOutputHeadV1>> heads;
    // Each mode object's head and index.
    std::vector<std::pair<Weak<ZwlrOutputModeV1>, std::pair<size_t, size_t>>> modes;
};

OutputManagement::OutputManagement(wl_display* display) : display_(display) {
    global_ = Global::create<ZwlrOutputManagerV1>(display, 4, [this](wl_client* client, uint32_t version, uint32_t id) {
        auto* r = make<ZwlrOutputManagerV1>(client, version, id);
        if (!r)
            return;
        auto c = std::make_unique<Client>();
        Client* cp = c.get();
        cp->resource = r;
        clients_.push_back(std::move(c));
        r->on_stop([](ZwlrOutputManagerV1* self) {
            self->send_finished();
            self->detach();
        });
        r->on_gone([this, cp] { std::erase_if(clients_, [cp](const auto& x) { return x.get() == cp; }); });
        r->on_create_configuration([this, cp](ZwlrOutputManagerV1* self, uint32_t id, uint32_t serial) {
            auto* conf = make<ZwlrOutputConfigurationV1>(self->client(), self->version(), id);
            if (!conf)
                return;
            struct State {
                std::vector<HeadConfig> heads;
                std::vector<std::string> named;
                bool used = false;
                uint32_t serial;
            };
            auto st = std::make_shared<State>();
            st->serial = serial;
            auto head_name = [this, cp](ZwlrOutputHeadV1* head_res) -> const Head* {
                for (size_t i = 0; i < cp->heads.size(); ++i)
                    if (cp->heads[i].get() == head_res && head_res && i < heads_.size())
                        return &heads_[i];
                return nullptr;
            };
            conf->on_enable_head([this, cp, st, head_name](ZwlrOutputConfigurationV1* self, uint32_t id,
                                                           ZwlrOutputHeadV1* head_res) {
                auto* ch = make<ZwlrOutputConfigurationHeadV1>(self->client(), self->version(), id);
                const Head* head = head_name(head_res);
                if (!ch)
                    return;
                if (!head) {
                    ch->detach();
                    return;
                }
                if (std::ranges::find(st->named, head->name) != st->named.end()) {
                    self->post_error(uint32_t(ZwlrOutputConfigurationV1::Error::AlreadyConfiguredHead),
                                     "the head is already configured");
                    return;
                }
                st->named.push_back(head->name);
                st->heads.push_back({head->name, true});
                const size_t index = st->heads.size() - 1;
                ch->on_set_mode([this, cp, st, index](ZwlrOutputConfigurationHeadV1*, ZwlrOutputModeV1* mode_res) {
                    for (auto& [w, at] : cp->modes)
                        if (w && w.get() == mode_res && at.first < heads_.size() &&
                            at.second < heads_[at.first].modes.size())
                            st->heads[index].mode = heads_[at.first].modes[at.second];
                });
                ch->on_set_custom_mode([st, index](ZwlrOutputConfigurationHeadV1* self, int32_t w, int32_t h,
                                                   int32_t refresh) {
                    if (w <= 0 || h <= 0 || refresh < 0) {
                        self->post_error(uint32_t(ZwlrOutputConfigurationHeadV1::Error::InvalidCustomMode),
                                         "a bad custom mode");
                        return;
                    }
                    st->heads[index].mode = Mode{w, h, refresh, false};
                });
                ch->on_set_position([st, index](ZwlrOutputConfigurationHeadV1*, int32_t x, int32_t y) {
                    st->heads[index].position = std::make_pair(x, y);
                });
                ch->on_set_transform([st, index](ZwlrOutputConfigurationHeadV1* self, int32_t t) {
                    if (t < 0 || t > 7) {
                        self->post_error(uint32_t(ZwlrOutputConfigurationHeadV1::Error::InvalidTransform),
                                         "no such transform");
                        return;
                    }
                    st->heads[index].transform = t;
                });
                ch->on_set_scale([st, index](ZwlrOutputConfigurationHeadV1* self, double scale) {
                    if (scale <= 0) {
                        self->post_error(uint32_t(ZwlrOutputConfigurationHeadV1::Error::InvalidScale),
                                         "the scale must be positive");
                        return;
                    }
                    st->heads[index].scale = scale;
                });
                ch->on_set_adaptive_sync([st, index](ZwlrOutputConfigurationHeadV1*, uint32_t state) {
                    st->heads[index].adaptive_sync = state != 0;
                });
            });
            conf->on_disable_head([st, head_name](ZwlrOutputConfigurationV1* self, ZwlrOutputHeadV1* head_res) {
                const Head* head = head_name(head_res);
                if (!head)
                    return;
                if (std::ranges::find(st->named, head->name) != st->named.end()) {
                    self->post_error(uint32_t(ZwlrOutputConfigurationV1::Error::AlreadyConfiguredHead),
                                     "the head is already configured");
                    return;
                }
                st->named.push_back(head->name);
                st->heads.push_back({head->name, false});
            });
            auto finish = [this, st](ZwlrOutputConfigurationV1* self, bool test_only) {
                if (std::exchange(st->used, true)) {
                    self->post_error(uint32_t(ZwlrOutputConfigurationV1::Error::AlreadyUsed),
                                     "the configuration was already used");
                    return;
                }
                // Made for screens that have changed since: it can't apply.
                if (st->serial != serial_) {
                    self->send_cancelled();
                    return;
                }
                Weak<ZwlrOutputConfigurationV1> w = self;
                Configuration c{st->heads, test_only, [w](bool ok) {
                                    if (auto* r = w.get()) {
                                        if (ok)
                                            r->send_succeeded();
                                        else
                                            r->send_failed();
                                    }
                                }};
                if (apply.empty())
                    c.done(false);
                else
                    apply.emit(c);
            };
            conf->on_apply([finish](ZwlrOutputConfigurationV1* self) { finish(self, false); });
            conf->on_test([finish](ZwlrOutputConfigurationV1* self) { finish(self, true); });
        });
        send_all(*cp);
    });
}

OutputManagement::~OutputManagement() {
    global_.reset();
    for (auto& c : clients_) {
        for (auto& h : c->heads)
            if (h)
                h->detach();
        for (auto& [m, at] : c->modes)
            if (m)
                m->detach();
        if (auto* r = c->resource.get()) {
            r->on_gone(nullptr);
            r->detach();
        }
    }
}

// Every head from scratch: old head and mode objects finish, new ones come.
void OutputManagement::send_all(Client& c) {
    auto* m = c.resource.get();
    if (!m || m->inert())
        return;
    for (auto& [w, at] : c.modes)
        if (auto* r = w.get()) {
            r->send_finished();
            r->detach();
        }
    for (auto& w : c.heads)
        if (auto* r = w.get()) {
            r->send_finished();
            r->detach();
        }
    c.modes.clear();
    c.heads.clear();
    const uint32_t v = m->version();
    for (size_t hi = 0; hi < heads_.size(); ++hi) {
        const Head& h = heads_[hi];
        auto* r = make<ZwlrOutputHeadV1>(m->client(), v, 0);
        if (!r)
            continue;
        m->send_head(r);
        c.heads.push_back(r);
        r->send_name(h.name.c_str());
        r->send_description(h.description.c_str());
        if (h.physical_width > 0 && h.physical_height > 0)
            r->send_physical_size(h.physical_width, h.physical_height);
        ZwlrOutputModeV1* current = nullptr;
        for (size_t mi = 0; mi < h.modes.size(); ++mi) {
            auto* mr = make<ZwlrOutputModeV1>(m->client(), std::min<uint32_t>(v, 3), 0);
            if (!mr)
                continue;
            r->send_mode(mr);
            mr->send_size(h.modes[mi].width, h.modes[mi].height);
            if (h.modes[mi].refresh > 0)
                mr->send_refresh(h.modes[mi].refresh);
            if (h.modes[mi].preferred)
                mr->send_preferred();
            c.modes.push_back({mr, {hi, mi}});
            if (int(mi) == h.current_mode)
                current = mr;
        }
        r->send_enabled(h.enabled ? 1 : 0);
        if (h.enabled) {
            if (current)
                r->send_current_mode(current);
            r->send_position(h.x, h.y);
            r->send_transform(h.transform);
            r->send_scale(h.scale);
        }
        if (v >= 2) {
            if (!h.make.empty())
                r->send_make(h.make.c_str());
            if (!h.model.empty())
                r->send_model(h.model.c_str());
            if (!h.serial.empty())
                r->send_serial_number(h.serial.c_str());
        }
        if (v >= 4)
            r->send_adaptive_sync(h.adaptive_sync ? 1 : 0);
    }
    m->send_done(serial_);
}

void OutputManagement::set_heads(std::vector<Head> heads) {
    if (heads == heads_)
        return;
    heads_ = std::move(heads);
    serial_ = wl_display_next_serial(display_);
    for (auto& c : clients_)
        send_all(*c);
}

// ---- output power -----------------------------------------------------------------------------

OutputPower::OutputPower(wl_display* display) {
    global_ = Global::create<ZwlrOutputPowerManagerV1>(display, 1, [this](wl_client* client, uint32_t version,
                                                                          uint32_t id) {
        auto* m = make<ZwlrOutputPowerManagerV1>(client, version, id);
        if (!m)
            return;
        m->on_get_output_power([this](ZwlrOutputPowerManagerV1* self, uint32_t id, wl_resource* output_res) {
            auto* r = make<ZwlrOutputPowerV1>(self->client(), self->version(), id);
            if (!r)
                return;
            Output* o = Output::from(output_res);
            if (!o) {
                r->send_failed();
                r->detach();
                return;
            }
            r->on_set_mode([this, o](ZwlrOutputPowerV1* self, uint32_t mode) {
                if (mode > 1) {
                    self->post_error(uint32_t(ZwlrOutputPowerV1::Error::InvalidMode), "no such mode");
                    return;
                }
                request_mode.emit(o, mode == 1);
            });
            std::erase_if(watches_, [](const Watch& w) { return !w.resource; });
            watches_.push_back({o, r});
            auto it = modes_.find(o);
            r->send_mode(it == modes_.end() || it->second ? 1 : 0);
        });
        std::erase_if(managers_, [](const auto& w) { return !w; });
        managers_.push_back(m);
    });
}

OutputPower::~OutputPower() {
    global_.reset();
    detach_all(managers_);
    for (auto& w : watches_)
        if (w.resource)
            w.resource->detach();
}

void OutputPower::set_mode(Output* output, bool on) {
    modes_[output] = on;
    for (auto& w : watches_)
        if (w.output == output)
            if (auto* r = static_cast<ZwlrOutputPowerV1*>(w.resource.get()); r && !r->inert())
                r->send_mode(on ? 1 : 0);
}

// ---- gamma --------------------------------------------------------------------------------------

GammaControls::GammaControls(wl_display* display) {
    global_ = Global::create<ZwlrGammaControlManagerV1>(display, 1, [this](wl_client* client, uint32_t version,
                                                                           uint32_t id) {
        auto* m = make<ZwlrGammaControlManagerV1>(client, version, id);
        if (!m)
            return;
        m->on_get_gamma_control([this](ZwlrGammaControlManagerV1* self, uint32_t id, wl_resource* output_res) {
            auto* r = make<ZwlrGammaControlV1>(self->client(), self->version(), id);
            if (!r)
                return;
            Output* o = Output::from(output_res);
            const uint32_t size = o && sizes_.contains(o) ? sizes_[o] : 0;
            // One control per screen: the next one fails until it goes.
            if (!o || !size || (controls_.contains(o) && controls_[o])) {
                r->send_failed();
                r->detach();
                return;
            }
            controls_[o] = r;
            r->on_set_gamma([this, o, size](ZwlrGammaControlV1* self, int fd) {
                std::vector<uint16_t> table(size_t(size) * 3);
                const size_t bytes = table.size() * sizeof(uint16_t);
                const ssize_t n = pread(fd, table.data(), bytes, 0);
                close(fd);
                if (n != ssize_t(bytes)) {
                    self->post_error(uint32_t(ZwlrGammaControlV1::Error::InvalidGamma), "the table is the wrong size");
                    return;
                }
                set_gamma.emit(o, table);
            });
            r->on_gone([this, o, r] {
                if (auto it = controls_.find(o); it != controls_.end() && it->second.get() == r) {
                    controls_.erase(it);
                    set_gamma.emit(o, {});  // back to the screen's own
                }
            });
            r->send_gamma_size(size);
        });
        std::erase_if(managers_, [](const auto& w) { return !w; });
        managers_.push_back(m);
    });
}

GammaControls::~GammaControls() {
    global_.reset();
    detach_all(managers_);
    for (auto& [o, w] : controls_)
        if (w)
            w->detach();
}

void GammaControls::set_size(Output* output, uint32_t size) {
    sizes_[output] = size;
    // A screen that went (size 0): its control fails.
    if (!size)
        if (auto it = controls_.find(output); it != controls_.end()) {
            if (auto* r = static_cast<ZwlrGammaControlV1*>(it->second.get())) {
                r->send_failed();
                r->detach();
            }
            controls_.erase(it);
        }
}

} // namespace atrium::wl
