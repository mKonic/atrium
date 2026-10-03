#include "wl/xdg_extras.hpp"

#include "wl/seat.hpp"

#include "appmenu-server.hpp"
#include "server-decoration-server.hpp"
#include "xdg-activation-v1-server.hpp"
#include "xdg-decoration-unstable-v1-server.hpp"
#include "xdg-dialog-v1-server.hpp"
#include "xdg-toplevel-tag-v1-server.hpp"

#include <algorithm>
#include <random>

namespace atrium::wl {

// ---- Decorations -------------------------------------------------------------------

Decorations::Decorations(wl_display* display, Mode kde_default) : kde_default_(kde_default) {
    xdg_global_ = Global::create<ZxdgDecorationManagerV1>(display, 1, [this](wl_client* client, uint32_t version,
                                                                            uint32_t id) {
        auto* m = make<ZxdgDecorationManagerV1>(client, version, id);
        if (!m)
            return;
        m->on_get_toplevel_decoration([this](ZxdgDecorationManagerV1* self, uint32_t id, wl_resource* toplevel_res) {
            Toplevel* t = Toplevel::from(toplevel_res);
            auto* r = make<ZxdgToplevelDecorationV1>(self->client(), self->version(), id);
            if (!r)
                return;
            if (!t) {
                r->detach();
                return;
            }
            if (xdg_for(t)) {
                r->post_error(uint32_t(ZxdgToplevelDecorationV1::Error::AlreadyConstructed),
                              "the toplevel already has a decoration object");
                return;
            }
            if (t->base() && t->base()->surface() && t->base()->surface()->buffer()) {
                r->post_error(uint32_t(ZxdgToplevelDecorationV1::Error::UnconfiguredBuffer),
                              "the toplevel already has a buffer");
                return;
            }
            auto owned = std::make_unique<Xdg>(Xdg{t});
            Xdg* d = owned.get();
            d->resource = r;
            xdg_.push_back(std::move(owned));
            auto drop = [this, d] {
                events.destroy_xdg.emit(d);
                if (Resource* res = d->resource.get())
                    res->detach();
                std::erase_if(xdg_, [d](const auto& x) { return x.get() == d; });
            };
            r->on_set_mode([this, d](ZxdgToplevelDecorationV1*, uint32_t mode) {
                d->requested = Mode(mode);
                events.request_mode.emit(d);
            });
            r->on_unset_mode([this, d](ZxdgToplevelDecorationV1*) {
                d->requested = Unset;
                events.request_mode.emit(d);
            });
            r->on_gone(drop);
            d->toplevel_gone = t->events.destroy.connect(drop);
            // Its mode goes out with each configure that carries a decision.
            if (t->base())
                d->configure = t->base()->events.configure.connect([d](uint32_t) {
                    auto* res = static_cast<ZxdgToplevelDecorationV1*>(d->resource.get());
                    if (res && d->scheduled != Unset)
                        res->send_configure(d->scheduled);
                });
            events.new_xdg.emit(d);
        });
        std::erase_if(managers_, [](const auto& w) { return !w; });
        managers_.push_back(m);
    });

    kde_global_ = Global::create<OrgKdeKwinServerDecorationManager>(display, 1, [this](wl_client* client,
                                                                                    uint32_t version, uint32_t id) {
        auto* m = make<OrgKdeKwinServerDecorationManager>(client, version, id);
        if (!m)
            return;
        m->on_create([this](OrgKdeKwinServerDecorationManager* self, uint32_t id, wl_resource* surface_res) {
            auto* r = make<OrgKdeKwinServerDecoration>(self->client(), self->version(), id);
            Surface* s = Surface::from(surface_res);
            if (!r)
                return;
            if (!s) {
                r->detach();
                return;
            }
            auto owned = std::make_unique<Kde>(Kde{s, kde_default_});
            Kde* d = owned.get();
            d->resource = r;
            kde_.push_back(std::move(owned));
            auto drop = [this, d] {
                events.destroy_kde.emit(d);
                if (Resource* res = d->resource.get())
                    res->detach();
                std::erase_if(kde_, [d](const auto& x) { return x.get() == d; });
            };
            r->on_request_mode([this, d](OrgKdeKwinServerDecoration* self, uint32_t mode) {
                if (mode > 2)
                    return;
                d->mode = Mode(mode);
                self->send_mode(mode);  // KDE's protocol: what was asked is what it gets
                events.kde_mode.emit(d);
            });
            r->on_gone(drop);
            d->surface_gone = s->events.destroy.connect(drop);
            r->send_mode(kde_default_);
            events.new_kde.emit(d);
        });
        std::erase_if(managers_, [](const auto& w) { return !w; });
        managers_.push_back(m);
        m->send_default_mode(kde_default_);
    });
}

Decorations::~Decorations() {
    xdg_global_.reset();
    kde_global_.reset();
    for (auto& m : managers_)
        if (m)
            m->detach();
    for (auto& d : xdg_)
        if (Resource* r = d->resource.get())
            r->detach();
    for (auto& d : kde_)
        if (Resource* r = d->resource.get())
            r->detach();
}

Decorations::Xdg* Decorations::xdg_for(Toplevel* toplevel) {
    for (auto& d : xdg_)
        if (d->toplevel == toplevel)
            return d.get();
    return nullptr;
}

Decorations::Kde* Decorations::kde_for(Surface* surface) {
    for (auto& d : kde_)
        if (d->surface == surface)
            return d.get();
    return nullptr;
}

void Decorations::set_mode(Xdg* d, Mode mode) {
    d->scheduled = mode;
    if (d->toplevel && d->toplevel->base())
        d->toplevel->base()->schedule_configure();
}

// ---- Dialogs ---------------------------------------------------------------------------

AppMenus::AppMenus(wl_display* display) {
    global_ = Global::create<OrgKdeKwinAppmenuManager>(display, 2, [this](wl_client* client, uint32_t version,
                                                                         uint32_t id) {
        auto* m = make<OrgKdeKwinAppmenuManager>(client, version, id);
        if (!m)
            return;
        m->on_create([this](OrgKdeKwinAppmenuManager* self, uint32_t id, wl_resource* surface_res) {
            auto* r = make<OrgKdeKwinAppmenu>(self->client(), self->version(), id);
            Surface* s = Surface::from(surface_res);
            if (!r)
                return;
            if (!s) {
                r->detach();
                return;
            }
            // One address a surface: a new object replaces the old one's.
            std::erase_if(menus_, [s](const auto& m) {
                if (m->surface != s)
                    return false;
                if (Resource* res = m->resource.get())
                    res->detach();
                return true;
            });
            auto owned = std::make_unique<Menu>(Menu{s, {}, r, {}});
            Menu* menu = owned.get();
            menus_.push_back(std::move(owned));
            auto drop = [this, menu] {
                Surface* surface = menu->surface;
                if (Resource* res = menu->resource.get())
                    res->detach();
                std::erase_if(menus_, [menu](const auto& x) { return x.get() == menu; });
                changed.emit(surface);
            };
            r->on_set_address([this, menu](OrgKdeKwinAppmenu*, const char* service, const char* path) {
                menu->address = {service ? service : "", path ? path : ""};
                changed.emit(menu->surface);
            });
            r->on_gone(drop);
            menu->surface_gone = s->events.destroy.connect([this, menu] {
                if (Resource* res = menu->resource.get())
                    res->detach();
                std::erase_if(menus_, [menu](const auto& x) { return x.get() == menu; });
            });
        });
        std::erase_if(managers_, [](const auto& w) { return !w; });
        managers_.push_back(m);
    });
}

AppMenus::~AppMenus() {
    global_.reset();
    for (auto& m : managers_)
        if (m)
            m->detach();
    for (auto& m : menus_)
        if (Resource* r = m->resource.get())
            r->detach();
}

const AppMenus::Address* AppMenus::for_surface(Surface* surface) const {
    for (const auto& m : menus_)
        if (m->surface == surface && !m->address.service.empty())
            return &m->address;
    return nullptr;
}

Dialogs::Dialogs(wl_display* display) {
    global_ = Global::create<XdgWmDialogV1>(display, 1, [this](wl_client* client, uint32_t version, uint32_t id) {
        auto* m = make<XdgWmDialogV1>(client, version, id);
        if (!m)
            return;
        m->on_get_xdg_dialog([this](XdgWmDialogV1* self, uint32_t id, wl_resource* toplevel_res) {
            auto* r = make<XdgDialogV1>(self->client(), self->version(), id);
            Toplevel* t = Toplevel::from(toplevel_res);
            if (!r)
                return;
            if (!t) {
                r->detach();
                return;
            }
            if (std::ranges::any_of(dialogs_, [t](const auto& d) { return d->toplevel == t; })) {
                self->post_error(uint32_t(XdgWmDialogV1::Error::AlreadyUsed), "the toplevel already has a dialog");
                return;
            }
            auto owned = std::make_unique<Dialog>(Dialog{t});
            Dialog* d = owned.get();
            d->resource = r;
            dialogs_.push_back(std::move(owned));
            auto drop = [this, d] {
                Toplevel* was = d->toplevel;
                const bool was_modal = d->modal;
                if (Resource* res = d->resource.get())
                    res->detach();
                std::erase_if(dialogs_, [d](const auto& x) { return x.get() == d; });
                if (was_modal)
                    changed.emit(was);
            };
            r->on_set_modal([this, d](XdgDialogV1*) {
                d->modal = true;
                changed.emit(d->toplevel);
            });
            r->on_unset_modal([this, d](XdgDialogV1*) {
                d->modal = false;
                changed.emit(d->toplevel);
            });
            r->on_gone(drop);
            d->toplevel_gone = t->events.destroy.connect(drop);
        });
        std::erase_if(managers_, [](const auto& w) { return !w; });
        managers_.push_back(m);
    });
}

Dialogs::~Dialogs() {
    global_.reset();
    for (auto& m : managers_)
        if (m)
            m->detach();
    for (auto& d : dialogs_)
        if (Resource* r = d->resource.get())
            r->detach();
}

bool Dialogs::modal(Toplevel* toplevel) const {
    for (const auto& d : dialogs_)
        if (d->toplevel == toplevel)
            return d->modal;
    return false;
}

// ---- Activation ---------------------------------------------------------------------------

namespace {

std::string random_token() {
    std::random_device rd;
    char out[33];
    for (int i = 0; i < 4; ++i)
        std::snprintf(out + i * 8, 9, "%08x", rd());
    return out;
}

} // namespace

Activation::Activation(wl_display* display, Seat& seat, std::chrono::milliseconds token_lifetime)
    : seat_(seat), lifetime_(token_lifetime) {
    global_ = Global::create<XdgActivationV1>(display, 1, [this](wl_client* client, uint32_t version, uint32_t id) {
        auto* m = make<XdgActivationV1>(client, version, id);
        if (!m)
            return;
        m->on_get_activation_token([this](XdgActivationV1* self, uint32_t id) {
            auto* r = make<XdgActivationTokenV1>(self->client(), self->version(), id);
            if (!r)
                return;
            auto pending = std::make_shared<Token>();
            auto committed = std::make_shared<bool>(false);
            r->on_set_serial([pending](XdgActivationTokenV1*, uint32_t serial, wl_resource* seat) {
                pending->serial = serial;
                pending->seat = Seat::from(seat);
            });
            r->on_set_app_id([pending](XdgActivationTokenV1*, const char* app_id) { pending->app_id = app_id; });
            r->on_set_surface([pending](XdgActivationTokenV1*, wl_resource* surface) {
                pending->surface = Surface::from(surface);
            });
            r->on_commit([this, pending, committed](XdgActivationTokenV1* self) {
                if (std::exchange(*committed, true)) {
                    self->post_error(uint32_t(XdgActivationTokenV1::Error::AlreadyUsed), "already committed");
                    return;
                }
                expire();
                auto t = std::make_unique<Token>();
                t->name = random_token();
                t->made = std::chrono::steady_clock::now();
                t->app_id = pending->app_id;
                t->surface = pending->surface;
                // Only a token from input this seat really delivered to the
                // asking client may take focus.
                if (pending->seat == &seat_ && seat_.validate_grab_serial(self->client(), pending->serial)) {
                    t->seat = &seat_;
                    t->serial = pending->serial;
                }
                Token* tp = t.get();
                if (tp->surface)
                    tp->surface_gone = tp->surface->events.destroy.connect([tp] { tp->surface = nullptr; });
                made_.push_back(std::move(t));
                self->send_done(tp->name.c_str());
            });
            std::erase_if(tokens_, [](const auto& w) { return !w; });
            tokens_.push_back(r);
        });
        m->on_activate([this](XdgActivationV1*, const char* name, wl_resource* surface_res) {
            Surface* s = Surface::from(surface_res);
            if (!s)
                return;
            expire();
            request_activate.emit({s, find(name)});
        });
        std::erase_if(managers_, [](const auto& w) { return !w; });
        managers_.push_back(m);
    });
}

Activation::~Activation() {
    global_.reset();
    for (auto* list : {&managers_, &tokens_})
        for (auto& w : *list)
            if (w)
                w->detach();
}

std::string Activation::make_token(const std::string& app_id) {
    expire();
    auto t = std::make_unique<Token>();
    t->name = random_token();
    t->made = std::chrono::steady_clock::now();
    t->app_id = app_id;
    t->seat = &seat_;  // atrium's own launches may take focus
    std::string name = t->name;
    made_.push_back(std::move(t));
    return name;
}

Activation::Token* Activation::find(const std::string& name) {
    for (auto& t : made_)
        if (t->name == name)
            return t.get();
    return nullptr;
}

void Activation::expire() {
    const auto now = std::chrono::steady_clock::now();
    std::erase_if(made_, [&](const auto& t) { return now - t->made > lifetime_; });
}

// ---- ToplevelTags ---------------------------------------------------------------------------

ToplevelTags::ToplevelTags(wl_display* display) {
    global_ = Global::create<XdgToplevelTagManagerV1>(display, 1, [this](wl_client* client, uint32_t version,
                                                                         uint32_t id) {
        auto* m = make<XdgToplevelTagManagerV1>(client, version, id);
        if (!m)
            return;
        m->on_set_toplevel_tag([this](XdgToplevelTagManagerV1*, wl_resource* toplevel, const char* tag) {
            if (Toplevel* t = Toplevel::from(toplevel))
                set_tag.emit({t, tag, {}});
        });
        m->on_set_toplevel_description([this](XdgToplevelTagManagerV1*, wl_resource* toplevel, const char* d) {
            if (Toplevel* t = Toplevel::from(toplevel))
                set_description.emit({t, {}, d});
        });
        std::erase_if(managers_, [](const auto& w) { return !w; });
        managers_.push_back(m);
    });
}

ToplevelTags::~ToplevelTags() {
    global_.reset();
    for (auto& m : managers_)
        if (m)
            m->detach();
}

} // namespace atrium::wl
