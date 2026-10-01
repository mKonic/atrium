// xdg-shell. The configure / ack / commit rules follow wlroots'
// types/xdg_shell (MIT), which clients have been tested against for years.
#include "wl/xdg_shell.hpp"

#include "wl/output.hpp"
#include "wl/seat.hpp"

#include <algorithm>

namespace atrium::wl {

namespace {

// The bounding box of a surface and its mapped subsurfaces, in its own
// coordinates.
void extend(const Surface* s, int dx, int dy, int& x1, int& y1, int& x2, int& y2) {
    const SurfaceState& c = s->current();
    x1 = std::min(x1, dx);
    y1 = std::min(y1, dy);
    x2 = std::max(x2, dx + c.width);
    y2 = std::max(y2, dy + c.height);
    for (const auto& p : s->children())
        if (p.sub && p.sub->surface() && p.sub->surface()->mapped())
            extend(p.sub->surface(), dx + p.x, dy + p.y, x1, y1, x2, y2);
}

Box extents(const Surface* s) {
    int x1 = 0, y1 = 0, x2 = 0, y2 = 0;
    extend(s, 0, 0, x1, y1, x2, y2);
    return {x1, y1, x2 - x1, y2 - y1};
}

Box intersect(const Box& a, const Box& b) {
    const int x1 = std::max(a.x, b.x), y1 = std::max(a.y, b.y);
    const int x2 = std::min(a.x + a.width, b.x + b.width), y2 = std::min(a.y + a.height, b.y + b.height);
    return x2 > x1 && y2 > y1 ? Box{x1, y1, x2 - x1, y2 - y1} : Box{};
}

bool has_content(const Surface* s) {
    return s->buffer() != nullptr;
}

} // namespace

// ---- Shell ------------------------------------------------------------------------

struct Shell::Client {
    Shell* shell;
    wl_client* client;
    Weak<XdgWmBase> base;
    std::vector<Weak<ShellSurface>> surfaces;
    uint32_t ping_serial = 0;
    wl_event_source* timer = nullptr;
};

Shell::Shell(wl_display* display, int ping_timeout_ms) : display_(display), ping_timeout_ms_(ping_timeout_ms) {
    global_ = Global::create<XdgWmBase>(display, 7, [this](wl_client* client, uint32_t version, uint32_t id) {
        auto* base = make<XdgWmBase>(client, version, id);
        if (!base)
            return;
        auto c = std::make_unique<Client>(Client{this, client, base});
        Client* cp = c.get();
        clients_.push_back(std::move(c));

        base->on_create_positioner([](XdgWmBase* self, uint32_t id) {
            make<Positioner>(self->client(), self->version(), id);
        });
        base->on_get_xdg_surface([this, cp](XdgWmBase* self, uint32_t id, wl_resource* surface_resource) {
            auto* surface = Surface::from(surface_resource);
            if (!surface)
                return;
            if ((surface->role_name() && surface->role_name() != ShellSurface::kRole) || surface->role()) {
                self->post_error(uint32_t(XdgWmBase::Error::Role), "the surface has another role");
                return;
            }
            if (has_content(surface) || surface->pending().buffer) {
                self->post_error(uint32_t(XdgSurface::Error::UnconfiguredBuffer),
                                 "an xdg_surface must not have a buffer at creation");
                return;
            }
            auto* ss = make<ShellSurface>(self->client(), self->version(), id, *this, surface);
            if (!ss)
                return;
            surface->set_role(ss, nullptr, 0);
            std::erase_if(cp->surfaces, [](const auto& w) { return !w; });
            cp->surfaces.push_back(ss);
            events.new_surface.emit(ss);
        });
        base->on_pong([cp](XdgWmBase*, uint32_t serial) {
            if (serial != cp->ping_serial)
                return;
            cp->ping_serial = 0;
            if (cp->timer) {
                wl_event_source_remove(cp->timer);
                cp->timer = nullptr;
            }
        });
        base->on_destroy([cp](XdgWmBase* self) {
            if (std::ranges::any_of(cp->surfaces, [](const auto& w) { return bool(w); }))
                self->post_error(uint32_t(XdgWmBase::Error::DefunctSurfaces),
                                 "xdg_wm_base destroyed before its surfaces");
        });
        base->on_gone([this, cp] {
            if (cp->timer)
                wl_event_source_remove(cp->timer);
            std::erase_if(clients_, [cp](const auto& c) { return c.get() == cp; });
        });
    });
}

Shell::~Shell() {
    global_.reset();
    for (auto& c : clients_) {
        if (c->timer)
            wl_event_source_remove(c->timer);
        if (c->base)
            c->base->detach();
        for (auto& s : c->surfaces)
            if (s)
                s->detach();
    }
    clients_.clear();
}

Shell::Client* Shell::client_of(wl_client* client) {
    for (auto& c : clients_)
        if (c->client == client)
            return c.get();
    return nullptr;
}

void Shell::ping(wl_client* client) {
    Client* c = client_of(client);
    if (!c || c->ping_serial || !c->base)
        return;
    c->ping_serial = wl_display_next_serial(display_);
    c->timer = wl_event_loop_add_timer(wl_display_get_event_loop(display_), [](void* data) {
        auto* c = static_cast<Client*>(data);
        wl_event_source_remove(c->timer);
        c->timer = nullptr;
        c->ping_serial = 0;
        c->shell->events.ping_timeout.emit(c->client);
        return 0;
    }, c);
    wl_event_source_timer_update(c->timer, ping_timeout_ms_);
    c->base->send_ping(c->ping_serial);
}

// ---- Positioner -----------------------------------------------------------------------

Positioner::Positioner(wl_client* client, uint32_t version, uint32_t id) : XdgPositioner(client, version, id) {
    on_set_size([this](XdgPositioner*, int32_t w, int32_t h) {
        if (w < 1 || h < 1) {
            post_error(uint32_t(Error::InvalidInput), "the size must be positive");
            return;
        }
        rules.width = w;
        rules.height = h;
    });
    on_set_anchor_rect([this](XdgPositioner*, int32_t x, int32_t y, int32_t w, int32_t h) {
        if (w < 0 || h < 0) {
            post_error(uint32_t(Error::InvalidInput), "the anchor rectangle can't be negative");
            return;
        }
        rules.anchor_rect = {x, y, w, h};
    });
    on_set_anchor([this](XdgPositioner*, uint32_t anchor) {
        if (anchor > PositionerRules::BottomRight) {
            post_error(uint32_t(Error::InvalidInput), "no such anchor");
            return;
        }
        rules.anchor = anchor;
    });
    on_set_gravity([this](XdgPositioner*, uint32_t gravity) {
        if (gravity > PositionerRules::BottomRight) {
            post_error(uint32_t(Error::InvalidInput), "no such gravity");
            return;
        }
        rules.gravity = gravity;
    });
    on_set_constraint_adjustment([this](XdgPositioner*, uint32_t adjust) {
        if (adjust > 63) {
            post_error(uint32_t(Error::InvalidInput), "no such constraint adjustment");
            return;
        }
        rules.constraint_adjustment = adjust;
    });
    on_set_offset([this](XdgPositioner*, int32_t x, int32_t y) {
        rules.offset_x = x;
        rules.offset_y = y;
    });
    on_set_reactive([this](XdgPositioner*) { rules.reactive = true; });
    on_set_parent_size([this](XdgPositioner*, int32_t w, int32_t h) {
        rules.has_parent_size = true;
        rules.parent_width = w;
        rules.parent_height = h;
    });
    on_set_parent_configure([this](XdgPositioner*, uint32_t serial) {
        rules.has_parent_configure = true;
        rules.parent_configure_serial = serial;
    });
}

// ---- ShellSurface ------------------------------------------------------------------------

ShellSurface::ShellSurface(wl_client* client, uint32_t version, uint32_t id, Shell& shell, Surface* surface)
    : XdgSurface(client, version, id), shell_(shell), surface_(surface) {
    surface_gone_ = surface->events.destroy.connect([this] { surface_gone(); });

    auto wm_error = [this](uint32_t code, const char* message) {
        if (Shell::Client* c = shell_.client_of(this->client()); c && c->base)
            c->base->post_error(code, message);
        else
            post_error(code, message);
    };
    on_destroy([this](XdgSurface*) {
        if (toplevel_ || popup_ || pip_)
            post_error(uint32_t(Error::DefunctRoleObject), "xdg_surface destroyed before its role object");
    });
    on_get_toplevel([this, wm_error](XdgSurface*, uint32_t id) {
        if ((kind_ != Kind::None && kind_ != Kind::Toplevel) || toplevel_ || popup_ || pip_) {
            wm_error(uint32_t(XdgWmBase::Error::Role), "the xdg_surface has another role");
            return;
        }
        auto* t = make<Toplevel>(this->client(), this->version(), id, this);
        if (!t)
            return;
        kind_ = Kind::Toplevel;
        toplevel_ = t;
        shell_.events.new_toplevel.emit(t);
    });
    on_get_popup([this, wm_error](XdgSurface*, uint32_t id, XdgSurface* parent_resource,
                                  XdgPositioner* positioner_resource) {
        auto* positioner = dynamic_cast<Positioner*>(positioner_resource);
        if (!positioner || !positioner->rules.complete()) {
            wm_error(uint32_t(XdgWmBase::Error::InvalidPositioner), "the positioner is incomplete");
            return;
        }
        if ((kind_ != Kind::None && kind_ != Kind::Popup) || toplevel_ || popup_ || pip_) {
            wm_error(uint32_t(XdgWmBase::Error::Role), "the xdg_surface has another role");
            return;
        }
        auto* parent = dynamic_cast<ShellSurface*>(parent_resource);
        auto* p = make<Popup>(this->client(), this->version(), id, this, parent ? parent->surface_ : nullptr,
                              positioner->rules);
        if (!p)
            return;
        kind_ = Kind::Popup;
        popup_ = p;
        if (parent)
            parent->events.new_popup.emit(p);
        shell_.events.new_popup.emit(p);
    });
    on_set_window_geometry([this](XdgSurface*, int32_t x, int32_t y, int32_t w, int32_t h) {
        if (kind_ == Kind::None) {
            post_error(uint32_t(Error::NotConstructed), "the xdg_surface has no role yet");
            return;
        }
        if (w <= 0 || h <= 0) {
            post_error(uint32_t(Error::InvalidSize), "the window geometry must have a size");
            return;
        }
        if (!surface_)
            return;
        auto& p = surface_->pending_state();
        p.xdg_geometry = {x, y, w, h};
        p.committed |= SurfaceState::XdgGeometry;
    });
    on_ack_configure([this, wm_error](XdgSurface*, uint32_t serial) {
        if (kind_ == Kind::None) {
            post_error(uint32_t(Error::NotConstructed), "the xdg_surface has no role yet");
            return;
        }
        auto it = std::ranges::find_if(sent_, [serial](const Sent& s) { return s.serial == serial; });
        if (it == sent_.end()) {
            wm_error(uint32_t(XdgWmBase::Error::InvalidSurfaceState), "no configure with that serial");
            return;
        }
        const Sent acked = *it;
        sent_.erase(sent_.begin(), it + 1);
        if (toplevel_)
            toplevel_->acked(acked);
        if (popup_)
            popup_->acked_ = acked.popup;
        if (pip_)
            pip_->acked_ = acked.pip;
        configured_ = true;
        if (surface_) {
            auto& p = surface_->pending_state();
            p.xdg_configure_serial = serial;
            p.committed |= SurfaceState::XdgAck;
        }
        events.ack_configure.emit(serial);
    });
}

ShellSurface::~ShellSurface() {
    if (idle_)
        wl_event_source_remove(idle_);
    idle_ = nullptr;
    if (toplevel_) {
        toplevel_->base_ = nullptr;
        toplevel_->gone();
    }
    if (popup_) {
        popup_->base_ = nullptr;
        popup_->gone();
    }
    if (pip_) {
        pip_->base_ = nullptr;
        pip_->gone();
    }
    for (Popup* p : std::vector(popups_))
        p->dismiss();
    events.destroy.emit();
    if (surface_) {
        surface_->unmap();
        surface_->clear_role(this);
    }
}

ShellSurface* ShellSurface::from(Surface* surface) {
    return surface ? dynamic_cast<ShellSurface*>(surface->role()) : nullptr;
}

void ShellSurface::surface_gone() {
    reset();
    if (toplevel_)
        toplevel_->gone();
    if (popup_)
        popup_->gone();
    if (pip_)
        pip_->gone();
    surface_ = nullptr;
}

void ShellSurface::reset() {
    configured_ = initialized_ = false;
    for (Popup* p : std::vector(popups_))
        p->dismiss();
    sent_.clear();
    if (idle_) {
        wl_event_source_remove(idle_);
        idle_ = nullptr;
    }
    if (toplevel_)
        toplevel_->reset();
    if (pip_)
        pip_->reset();
}

uint32_t ShellSurface::schedule_configure() {
    if (!initialized_ || !surface_)
        return 0;
    if (!idle_) {
        scheduled_serial_ = wl_display_next_serial(shell_.display());
        idle_ = wl_event_loop_add_idle(wl_display_get_event_loop(shell_.display()), [](void* data) {
            static_cast<ShellSurface*>(data)->flush_configure();
        }, this);
    }
    return scheduled_serial_;
}

void ShellSurface::flush_configure() {
    idle_ = nullptr;
    Sent s{scheduled_serial_, {}, {}, {}};
    if (toplevel_)
        toplevel_->sent(s);
    if (pip_)
        pip_->sent(s);
    if (popup_) {
        if (popup_->reposition_token_ && popup_->version() >= 3)
            popup_->send_repositioned(*popup_->reposition_token_);
        popup_->reposition_token_.reset();
        const Box& g = popup_->scheduled_;
        popup_->send_configure(g.x, g.y, g.width, g.height);
        s.popup = g;
    }
    sent_.push_back(s);
    events.configure.emit(s.serial);
    send_configure(s.serial);
}

void ShellSurface::ping() {
    shell_.ping(client());
}

bool ShellSurface::precommit(Surface& s) {
    const SurfaceState& p = s.pending();
    if ((p.committed & SurfaceState::Buffer) && p.buffer && !configured_) {
        post_error(uint32_t(Error::UnconfiguredBuffer), "a buffer before the first configure was acked");
        return false;
    }
    if (!toplevel_ && !popup_ && !pip_) {
        post_error(uint32_t(Error::NotConstructed), "the xdg_surface has no role object");
        return false;
    }
    if (toplevel_ && (p.committed & SurfaceState::XdgSizeLimits)) {
        if (p.min_width < 0 || p.min_height < 0 || p.max_width < 0 || p.max_height < 0 ||
            (p.max_width && p.max_width < p.min_width) || (p.max_height && p.max_height < p.min_height)) {
            toplevel_->post_error(uint32_t(XdgToplevel::Error::InvalidSize), "an invalid min or max size");
            return false;
        }
    }
    return true;
}

void ShellSurface::commit(Surface& s) {
    const bool unmap_commit = (s.current().committed & SurfaceState::Buffer) && !has_content(&s);
    if (unmap_commit) {
        s.unmap();
        reset();
        return;
    }
    const bool initial = !initialized_;
    initialized_ = true;
    if (toplevel_) {
        toplevel_->committed();
        if (initial) {
            toplevel_->events.initial_commit.emit();
            schedule_configure();
        }
    } else if (popup_) {
        popup_->current_ = popup_->acked_;
        if (initial)
            schedule_configure();
    } else if (pip_) {
        pip_->committed();
        if (initial) {
            pip_->events.initial_commit.emit();
            schedule_configure();
        }
    } else {
        return;
    }
    update_geometry();
    // Past the bounds it was sent: an error (xx_pip_v1.configure_bounds).
    if (pip_ && pip_->sent_bounds_ && has_content(&s) &&
        (geometry_.width > pip_->sent_bounds_->first || geometry_.height > pip_->sent_bounds_->second)) {
        pip_->post_error(uint32_t(XxPipV1::Error::InvalidSize), "the surface is bigger than its bounds");
        return;
    }
    if (!s.mapped() && has_content(&s) && configured_)
        s.map();
}

void ShellSurface::update_geometry() {
    if (!surface_)
        return;
    const auto& g = surface_->current().xdg_geometry;
    const Box ext = extents(surface_);
    if (g.width > 0 && g.height > 0) {
        const Box set{g.x, g.y, g.width, g.height};
        geometry_ = intersect(ext, set);
        if (geometry_.empty())
            geometry_ = set;  // extents empty (no buffer yet): what was asked
    } else {
        geometry_ = ext;
    }
}

// ---- Toplevel -------------------------------------------------------------------------------

Toplevel::Toplevel(wl_client* client, uint32_t version, uint32_t id, ShellSurface* base)
    : XdgToplevel(client, version, id), base_(base) {
    auto configured = [this]() {
        if (base_ && base_->configured())
            return true;
        if (base_)
            base_->post_error(uint32_t(XdgSurface::Error::NotConstructed), "the surface isn't configured yet");
        return false;
    };
    on_set_parent([this](XdgToplevel*, XdgToplevel* parent) {
        if (!set_parent(dynamic_cast<Toplevel*>(parent)))
            post_error(uint32_t(Error::InvalidParent), "a toplevel can't be its own ancestor");
    });
    on_set_title([this](XdgToplevel*, const char* title) {
        title_ = title;
        events.set_title.emit();
    });
    on_set_app_id([this](XdgToplevel*, const char* app_id) {
        app_id_ = app_id;
        events.set_app_id.emit();
    });
    on_show_window_menu([this, configured](XdgToplevel*, wl_resource* seat, uint32_t serial, int32_t x, int32_t y) {
        if (configured())
            events.request_window_menu.emit({Seat::from(seat), serial, x, y});
    });
    on_move([this, configured](XdgToplevel*, wl_resource* seat, uint32_t serial) {
        if (configured())
            events.request_move.emit({Seat::from(seat), serial});
    });
    on_resize([this, configured](XdgToplevel*, wl_resource* seat, uint32_t serial, uint32_t edges) {
        if (edges > uint32_t(ResizeEdge::BottomRight) || edges == 3 || edges == 7) {
            post_error(uint32_t(Error::InvalidResizeEdge), "no such edge");
            return;
        }
        if (configured())
            events.request_resize.emit({Seat::from(seat), serial, edges});
    });
    auto limits = [this](int w, int h, bool max) {
        if (w < 0 || h < 0) {
            post_error(uint32_t(Error::InvalidSize), "a size can't be negative");
            return;
        }
        if (!base_ || !base_->surface())
            return;
        auto& p = base_->surface()->pending_state();
        (max ? p.max_width : p.min_width) = w;
        (max ? p.max_height : p.min_height) = h;
        p.committed |= SurfaceState::XdgSizeLimits;
    };
    on_set_max_size([limits](XdgToplevel*, int32_t w, int32_t h) { limits(w, h, true); });
    on_set_min_size([limits](XdgToplevel*, int32_t w, int32_t h) { limits(w, h, false); });
    on_set_maximized([this](XdgToplevel*) {
        requested_.maximized = true;
        events.request_maximize.emit();
    });
    on_unset_maximized([this](XdgToplevel*) {
        requested_.maximized = false;
        events.request_maximize.emit();
    });
    on_set_fullscreen([this](XdgToplevel*, wl_resource* output) {
        requested_.fullscreen = true;
        requested_.fullscreen_output = output ? Output::from(output) : nullptr;
        events.request_fullscreen.emit();
    });
    on_unset_fullscreen([this](XdgToplevel*) {
        requested_.fullscreen = false;
        requested_.fullscreen_output = nullptr;
        events.request_fullscreen.emit();
    });
    on_set_minimized([this](XdgToplevel*) {
        requested_.minimized = true;
        events.request_minimize.emit();
    });
}

Toplevel::~Toplevel() {
    gone();
    if (base_) {
        base_->toplevel_ = nullptr;
        if (Surface* s = base_->surface())
            s->unmap();
        base_->reset();
    }
}

void Toplevel::gone() {
    if (gone_)
        return;
    gone_ = true;
    parent_unmap_.disconnect();
    parent_destroy_.disconnect();
    parent_ = nullptr;
    events.destroy.emit();
}

Toplevel* Toplevel::from(wl_resource* resource) {
    return dynamic_cast<Toplevel*>(XdgToplevel::from(resource));
}

Toplevel* Toplevel::from(Surface* surface) {
    ShellSurface* s = ShellSurface::from(surface);
    return s ? s->toplevel() : nullptr;
}

int Toplevel::min_width() const {
    return base_ && base_->surface() ? base_->surface()->current().min_width : 0;
}
int Toplevel::min_height() const {
    return base_ && base_->surface() ? base_->surface()->current().min_height : 0;
}
int Toplevel::max_width() const {
    return base_ && base_->surface() ? base_->surface()->current().max_width : 0;
}
int Toplevel::max_height() const {
    return base_ && base_->surface() ? base_->surface()->current().max_height : 0;
}

bool Toplevel::set_parent(Toplevel* parent) {
    for (Toplevel* t = parent; t; t = t->parent_)
        if (t == this)
            return false;
    parent_unmap_.disconnect();
    parent_destroy_.disconnect();
    // Only a shown window is a parent; one that goes hands its children up.
    if (parent && parent->base_ && parent->base_->surface() && parent->base_->surface()->mapped()) {
        parent_ = parent;
        parent_unmap_ = parent->base_->surface()->events.unmap.connect([this] { set_parent(parent_->parent_); });
        parent_destroy_ = parent->events.destroy.connect([this] { set_parent(nullptr); });
    } else {
        parent_ = nullptr;
    }
    events.set_parent.emit();
    return true;
}

void Toplevel::reset() {
    set_parent(nullptr);
    requested_ = {};
    scheduled_ = acked_ = current_ = {};
}

uint32_t Toplevel::set_size(int width, int height) {
    scheduled_.width = width;
    scheduled_.height = height;
    return base_ ? base_->schedule_configure() : 0;
}
uint32_t Toplevel::set_activated(bool on) {
    scheduled_.activated = on;
    return base_ ? base_->schedule_configure() : 0;
}
uint32_t Toplevel::set_maximized(bool on) {
    scheduled_.maximized = on;
    return base_ ? base_->schedule_configure() : 0;
}
uint32_t Toplevel::set_fullscreen(bool on) {
    scheduled_.fullscreen = on;
    return base_ ? base_->schedule_configure() : 0;
}
uint32_t Toplevel::set_resizing(bool on) {
    scheduled_.resizing = on;
    return base_ ? base_->schedule_configure() : 0;
}
uint32_t Toplevel::set_tiled(uint32_t edges) {
    scheduled_.tiled = edges;
    return base_ ? base_->schedule_configure() : 0;
}
uint32_t Toplevel::set_constrained(uint32_t edges) {
    scheduled_.constrained = edges;
    return base_ ? base_->schedule_configure() : 0;
}
uint32_t Toplevel::set_suspended(bool on) {
    scheduled_.suspended = on;
    return base_ ? base_->schedule_configure() : 0;
}
uint32_t Toplevel::set_bounds(int width, int height) {
    bounds_ = {width, height};
    return base_ ? base_->schedule_configure() : 0;
}
uint32_t Toplevel::set_wm_capabilities(uint32_t caps) {
    wm_caps_ = caps;
    return base_ ? base_->schedule_configure() : 0;
}

void Toplevel::sent(ShellSurface::Sent& s) {
    const uint32_t v = version();
    if (bounds_ && v >= 4)
        send_configure_bounds(bounds_->first, bounds_->second);
    bounds_.reset();
    if (wm_caps_ && v >= 5) {
        std::vector<uint32_t> caps;
        using C = WmCapabilities;
        if (*wm_caps_ & 1)
            caps.push_back(uint32_t(C::WindowMenu));
        if (*wm_caps_ & 2)
            caps.push_back(uint32_t(C::Maximize));
        if (*wm_caps_ & 4)
            caps.push_back(uint32_t(C::Fullscreen));
        if (*wm_caps_ & 8)
            caps.push_back(uint32_t(C::Minimize));
        wl_array a{caps.size() * sizeof(uint32_t), caps.size() * sizeof(uint32_t), caps.data()};
        send_wm_capabilities(&a);
    }
    wm_caps_.reset();

    const ToplevelState& c = scheduled_;
    std::vector<uint32_t> states;
    using S = State;
    if (c.maximized)
        states.push_back(uint32_t(S::Maximized));
    if (c.fullscreen)
        states.push_back(uint32_t(S::Fullscreen));
    if (c.resizing)
        states.push_back(uint32_t(S::Resizing));
    if (c.activated)
        states.push_back(uint32_t(S::Activated));
    if (v >= 2) {
        if (c.tiled & 4)
            states.push_back(uint32_t(S::TiledLeft));
        if (c.tiled & 8)
            states.push_back(uint32_t(S::TiledRight));
        if (c.tiled & 1)
            states.push_back(uint32_t(S::TiledTop));
        if (c.tiled & 2)
            states.push_back(uint32_t(S::TiledBottom));
    }
    if (c.suspended && v >= 6)
        states.push_back(uint32_t(S::Suspended));
    if (v >= 7) {
        if (c.constrained & 4)
            states.push_back(uint32_t(S::ConstrainedLeft));
        if (c.constrained & 8)
            states.push_back(uint32_t(S::ConstrainedRight));
        if (c.constrained & 1)
            states.push_back(uint32_t(S::ConstrainedTop));
        if (c.constrained & 2)
            states.push_back(uint32_t(S::ConstrainedBottom));
    }
    wl_array a{states.size() * sizeof(uint32_t), states.size() * sizeof(uint32_t), states.data()};
    send_configure(c.width, c.height, &a);
    s.toplevel = c;
}

void Toplevel::acked(const ShellSurface::Sent& s) {
    acked_ = s.toplevel;
}

void Toplevel::committed() {
    current_ = acked_;
}

// ---- Popup ----------------------------------------------------------------------------------

Popup::Popup(wl_client* client, uint32_t version, uint32_t id, ShellSurface* base, Surface* parent,
             const PositionerRules& rules)
    : XdgPopup(client, version, id), base_(base), rules_(rules) {
    scheduled_ = rules.geometry();
    set_parent(parent);
    on_grab([this](XdgPopup*, wl_resource* seat, uint32_t serial) {
        events.request_grab.emit({Seat::from(seat), serial});
    });
    on_reposition([this](XdgPopup*, XdgPositioner* positioner_resource, uint32_t token) {
        auto* positioner = dynamic_cast<Positioner*>(positioner_resource);
        if (!positioner || !positioner->rules.complete())
            return;
        rules_ = positioner->rules;
        reposition_token_ = token;
        scheduled_ = rules_.geometry();
        if (events.reposition.empty()) {
            if (base_)
                base_->schedule_configure();
        } else {
            events.reposition.emit();
        }
    });
    on_destroy([this](XdgPopup*) {
        // Popups close top first.
        if (base_ && std::ranges::any_of(base_->popups(), [](Popup* p) { return !p->dismissed_; })) {
            if (Shell::Client* c = base_->shell_.client_of(this->client()); c && c->base)
                c->base->post_error(uint32_t(XdgWmBase::Error::NotTheTopmostPopup),
                                    "a popup was destroyed before its child popups");
        }
    });
}

Popup::~Popup() {
    gone();
    if (base_) {
        base_->popup_ = nullptr;
        if (Surface* s = base_->surface())
            s->unmap();
        base_->reset();
    }
}

void Popup::set_parent(Surface* parent) {
    if (ShellSurface* old = ShellSurface::from(parent_))
        std::erase(old->popups_, this);
    parent_gone_.disconnect();
    parent_ = parent;
    if (!parent)
        return;
    if (ShellSurface* p = ShellSurface::from(parent))
        p->popups_.push_back(this);
    parent_gone_ = parent->events.destroy.connect([this] {
        parent_ = nullptr;
        dismiss();
    });
}

void Popup::gone() {
    if (gone_)
        return;
    gone_ = true;
    if (ShellSurface* p = ShellSurface::from(parent_))
        std::erase(p->popups_, this);
    parent_gone_.disconnect();
    events.destroy.emit();
}

void Popup::unconstrain_from(const Box& box) {
    Box b = rules_.geometry();
    rules_.unconstrain(box, b);
    scheduled_ = b;
    if (base_)
        base_->schedule_configure();
}

void Popup::dismiss() {
    if (dismissed_)
        return;
    dismissed_ = true;
    // Its own popups go first: the protocol closes them top down.
    if (base_)
        for (Popup* child : std::vector(base_->popups_))
            child->dismiss();
    send_popup_done();
    if (base_ && base_->surface())
        base_->surface()->unmap();
    gone();
}

// ---- Pip ---------------------------------------------------------------------------------

Pip::Pip(wl_client* client, uint32_t version, uint32_t id, ShellSurface* base)
    : XxPipV1(client, version, id), base_(base) {
    auto configured = [this]() {
        if (base_ && base_->configured())
            return true;
        if (base_)
            base_->post_error(uint32_t(XdgSurface::Error::NotConstructed), "the surface isn't configured yet");
        return false;
    };
    on_set_app_id([this](XxPipV1*, const char* app_id) {
        app_id_ = app_id;
        events.set_app_id.emit();
    });
    on_set_origin([this](XxPipV1*, wl_resource* origin) {
        Surface* o = Surface::from(origin);
        if (base_ && o == base_->surface()) {
            post_error(uint32_t(Error::InvalidOrigin), "a pip can't be its own origin");
            return;
        }
        pending_origin_gone_.disconnect();
        pending_origin_ = o;
        origin_pending_ = true;
        if (o)
            pending_origin_gone_ = o->events.destroy.connect([this] {
                pending_origin_ = nullptr;
                pending_origin_gone_.disconnect();
            });
    });
    on_set_origin_rect([this](XxPipV1*, int32_t x, int32_t y, uint32_t w, uint32_t h) {
        if (w == 0 || h == 0) {
            post_error(uint32_t(Error::InvalidOrigin), "an origin rect needs a size");
            return;
        }
        pending_origin_rect_ = Box{x, y, int(w), int(h)};
        origin_rect_pending_ = true;
    });
    on_move([this, configured](XxPipV1*, wl_resource* seat, uint32_t serial) {
        if (configured())
            events.request_move.emit({Seat::from(seat), serial});
    });
    on_resize([this, configured](XxPipV1*, wl_resource* seat, uint32_t serial, uint32_t edges) {
        if (edges > uint32_t(ResizeEdge::BottomRight) || edges == 3 || edges == 7) {
            post_error(uint32_t(Error::InvalidResizeEdge), "no such edge");
            return;
        }
        if (configured())
            events.request_resize.emit({Seat::from(seat), serial, edges});
    });
}

Pip::~Pip() {
    gone();
    if (base_) {
        base_->pip_ = nullptr;
        if (Surface* s = base_->surface())
            s->unmap();
        base_->reset();
    }
}

void Pip::gone() {
    if (gone_)
        return;
    gone_ = true;
    pending_origin_gone_.disconnect();
    origin_gone_.disconnect();
    pending_origin_ = origin_ = nullptr;
    events.destroy.emit();
}

Pip* Pip::from(Surface* surface) {
    ShellSurface* s = ShellSurface::from(surface);
    return s ? s->pip() : nullptr;
}

void Pip::reset() {
    // Unmapped: back to how it was made (the protocol's words).
    scheduled_ = acked_ = current_ = {};
    bounds_.reset();
    sent_bounds_.reset();
}

uint32_t Pip::set_size(int width, int height) {
    scheduled_ = {width, height};
    return base_ ? base_->schedule_configure() : 0;
}

uint32_t Pip::set_bounds(int width, int height) {
    bounds_ = {width, height};
    return base_ ? base_->schedule_configure() : 0;
}

void Pip::sent(ShellSurface::Sent& s) {
    if (bounds_) {
        send_configure_bounds(bounds_->first, bounds_->second);
        sent_bounds_ = bounds_;
        bounds_.reset();
    }
    send_configure_size(scheduled_.first, scheduled_.second);
    s.pip = scheduled_;
}

void Pip::committed() {
    current_ = acked_;
    if (origin_pending_) {
        origin_pending_ = false;
        origin_gone_.disconnect();
        origin_ = pending_origin_;
        if (origin_)
            origin_gone_ = origin_->events.destroy.connect([this] {
                origin_ = nullptr;
                origin_gone_.disconnect();
            });
    }
    if (origin_rect_pending_) {
        origin_rect_pending_ = false;
        origin_rect_ = pending_origin_rect_;
    }
}

// ---- PipShell ----------------------------------------------------------------------------

PipShell::PipShell(wl_display* display) {
    global_ = Global::create<XxPipShellV1>(display, 1, [this](wl_client* client, uint32_t version, uint32_t id) {
        auto* m = make<XxPipShellV1>(client, version, id);
        if (!m)
            return;
        m->on_get_pip([this](XxPipShellV1* self, uint32_t id, wl_resource* xdg_surface) {
            auto* base = dynamic_cast<ShellSurface*>(XdgSurface::from(xdg_surface));
            if (!base || base->kind_ != ShellSurface::Kind::None || base->toplevel_ || base->popup_ || base->pip_) {
                self->post_error(uint32_t(XxPipShellV1::Error::Role), "the xdg_surface has another role");
                return;
            }
            Surface* s = base->surface();
            if (base->initialized_ || (s && (s->buffer() || s->pending().buffer))) {
                self->post_error(uint32_t(XxPipShellV1::Error::AlreadyConstructed),
                                 "the surface already has a buffer or was committed");
                return;
            }
            auto* p = make<Pip>(self->client(), self->version(), id, base);
            if (!p)
                return;
            base->kind_ = ShellSurface::Kind::Pip;
            base->pip_ = p;
            events.new_pip.emit(p);
        });
        std::erase_if(managers_, [](const auto& w) { return !w; });
        managers_.push_back(m);
    });
}

PipShell::~PipShell() {
    global_.reset();
    for (auto& w : managers_)
        if (w)
            w->detach();
}

} // namespace atrium::wl
