#include "output.hpp"
#include "layer_surface.hpp"
#include "seat.hpp"
#include "server.hpp"
#include "surface_blur.hpp"
#include "view.hpp"
#include "geometry.hpp"
#include "rules.hpp"

#include <cmath>

namespace atrium {

XdgView::XdgView(Server& srv, wl::Toplevel* t) : View(srv, Kind::Xdg), toplevel(t) {
    toplevel->data = this;
    wl::Surface* s = toplevel->base()->surface();
    auto& c = connections_;

    c.push_back(s->events.commit.connect([this] { commit(); }));
    c.push_back(s->events.map.connect([this] {
        const Box g = toplevel->base()->geometry();
        geom.width = g.width;
        geom.height = g.height;
        handle_map();
        if (toplevel->requested().fullscreen)
            set_fullscreen(true);
        else if (toplevel->requested().maximized)
            set_maximized(true);
    }));
    c.push_back(s->events.unmap.connect([this] { handle_unmap(); }));
    c.push_back(toplevel->events.destroy.connect([this] { delete this; }));
    c.push_back(toplevel->events.initial_commit.connect([this] { initial_commit(); }));

    // xdg-shell wants a configure in reply to every state request, even one
    // the compositor ignores or that changes nothing.
    auto reply = [this] {
        if (toplevel->base() && toplevel->base()->initialized())
            toplevel->base()->schedule_configure();
    };
    c.push_back(toplevel->events.request_fullscreen.connect([this, reply] {
        if (mapped && toplevel->requested().fullscreen != fullscreen)
            set_fullscreen(toplevel->requested().fullscreen);
        else
            reply();
    }));
    c.push_back(toplevel->events.request_maximize.connect([this, reply] {
        if (mapped && !fullscreen && !layout_owned() && toplevel->requested().maximized != maximized)
            set_maximized(toplevel->requested().maximized);
        else
            reply();
    }));
    c.push_back(toplevel->events.request_minimize.connect([this] {
        if (mapped && !layout_owned())
            request_minimized(true);
    }));
    c.push_back(toplevel->events.request_move.connect([this](const wl::Toplevel::MoveRequest& e) {
        if (mapped && !layout_owned() && server.wl->seat->validate_grab_serial(toplevel->client(), e.serial))
            server.seat->begin_move(this);
    }));
    c.push_back(toplevel->events.request_resize.connect([this](const wl::Toplevel::ResizeRequest& e) {
        // xdg_toplevel's resize edges are Edges' bits.
        if (mapped && !layout_owned() && server.wl->seat->validate_grab_serial(toplevel->client(), e.serial))
            server.seat->begin_resize(this, e.edges);
    }));
    // A right-click on a GTK header bar: atrium's window menu.
    c.push_back(toplevel->events.request_window_menu.connect([this](const wl::Toplevel::MenuRequest& e) {
        if (!mapped)
            return;
        double ox, oy;
        surface_origin(ox, oy);
        server.show_window_menu(this, ox + e.x, oy + e.y);
    }));
    c.push_back(toplevel->events.set_title.connect([this] { update_title(); }));
    c.push_back(toplevel->events.set_app_id.connect([this] { update_title(); }));

    // Decorations announced before this toplevel existed.
    if (auto* d = server.wl->decorations->kde_for(s))
        set_kde_decoration(d);
    if (auto* d = server.wl->decorations->xdg_for(toplevel))
        set_decoration(d);
}

XdgView::~XdgView() {
    toplevel->data = nullptr;
}

void XdgView::initial_commit() {
    toplevel->set_wm_capabilities(2 | 4 | 8);  // maximize, fullscreen, minimize
    wl::Surface* s = surface();
    if (Output* o = server.focused_output) {
        server.wl->fractional_scales->set_preferred_scale(s, o->screen->scale);
        s->set_preferred_scale(int32_t(std::ceil(o->screen->scale)));
        // Tell the client how much room there is before it picks a size.
        toplevel->set_bounds(o->usable.width, o->usable.height - (wants_ssd() ? Titlebar::kHeight : 0));
    }
    apply_decoration_mode();
    // A rule sends it to a secret space: start at the size it will have
    // there. Otherwise reopen at the size the app last had, or let it pick
    // its own.
    remembered_ = server.placement_for(this);
    const bool secret = server.focused_output && !toplevel->app_id().empty() &&
        !apply_rules(server.config.rules, toplevel->app_id(), toplevel->title()).secret.empty();
    if (secret && !toplevel->parent()) {
        const Box f = geometry::secret_frame(server.focused_output->usable, server.config.secret_margin);
        toplevel->set_size(f.width, std::max(1, f.height - (wants_ssd() ? Titlebar::kHeight : 0)));
    } else if (remembered_) {
        toplevel->set_size(remembered_->width, std::max(1, remembered_->height - (wants_ssd() ? Titlebar::kHeight : 0)));
    } else {
        toplevel->set_size(0, 0);
    }
}

void XdgView::commit() {
    if (!mapped)
        return;
    // Crop to the window geometry: client-side shadows are the client's
    // business, ours are drawn by the compositor.
    const Box g = toplevel->base()->geometry();
    const Box clip{g.x, g.y, g.width, g.height};
    scene::subsurface_tree_set_clip(content, &clip);
    handle_size(g.width, g.height);
    if (resize_settling_ && !awaiting_configure())
        settle_resize();
    update_corners();
}

bool XdgView::awaiting_configure() const {
    // Serials wrap; compare by signed distance.
    return last_size_serial_ && toplevel->base() &&
           int32_t(toplevel->base()->configure_serial() - last_size_serial_) < 0;
}

void XdgView::configure(const Box& frame) {
    if (!toplevel->base() || !toplevel->base()->initialized())
        return;
    const Box box = content_box(frame);
    if (box.width != toplevel->scheduled().width || box.height != toplevel->scheduled().height)
        last_size_serial_ = toplevel->set_size(box.width, box.height);
}

scene::Tree* XdgView::create_content(scene::Tree* parent) {
    return scene::xdg_surface_create(parent, toplevel->base());
}

void XdgView::surface_origin(double& x, double& y) const {
    const Box g = toplevel->base()->geometry();
    x = geom.x - g.x;
    y = geom.y + top() - g.y;
}

const char* XdgView::app_id() const {
    return toplevel->app_id().c_str();
}

const char* XdgView::title() const {
    return toplevel->title().c_str();
}

View* XdgView::parent() const {
    return toplevel->parent() ? static_cast<View*>(toplevel->parent()->data) : nullptr;
}

void XdgView::size_hints(Box& min, Box& max) const {
    min = {0, 0, toplevel->min_width(), toplevel->min_height()};
    max = {0, 0, toplevel->max_width(), toplevel->max_height()};
}

bool XdgView::is_dialog() const {
    Box min, max;
    size_hints(min, max);
    return toplevel->parent() ||
           (min.width > 0 && min.height > 0 && (min.width == max.width || min.height == max.height));
}

bool XdgView::modal() const {
    return toplevel->parent() && server.wl->dialogs->modal(toplevel);
}

void XdgView::close() {
    toplevel->close();
}

void XdgView::send_activated(bool a) {
    toplevel->set_activated(a);
}

void XdgView::send_maximized(bool m) {
    toplevel->set_maximized(m);
}

void XdgView::send_fullscreen(bool f) {
    toplevel->set_fullscreen(f);
}

void XdgView::send_suspended(bool s) {
    toplevel->set_suspended(s);
}

void XdgView::dismiss_popups() {
    if (!toplevel->base())
        return;
    const std::vector<wl::Popup*> popups = toplevel->base()->popups();
    for (wl::Popup* p : popups)
        p->dismiss();
}

// --- decorations -------------------------------------------------------------

void XdgView::set_decoration(wl::Decorations::Xdg* d) {
    decoration_ = d;
    auto& decorations = *server.wl->decorations;
    decoration_request_ = decorations.events.request_mode.connect([this](wl::Decorations::Xdg* x) {
        if (x == decoration_)
            apply_decoration_mode();
    });
    decoration_destroy_ = decorations.events.destroy_xdg.connect([this](wl::Decorations::Xdg* x) {
        if (x != decoration_)
            return;
        decoration_ = nullptr;
        decoration_request_.disconnect();
        decoration_destroy_.disconnect();
        refresh_decoration_mode();
    });
    apply_decoration_mode();
}

void XdgView::set_kde_decoration(wl::Decorations::Kde* d) {
    kde_decoration_ = d;
    auto& decorations = *server.wl->decorations;
    kde_mode_ = decorations.events.kde_mode.connect([this](wl::Decorations::Kde* k) {
        if (k == kde_decoration_)
            refresh_decoration_mode();
    });
    kde_destroy_ = decorations.events.destroy_kde.connect([this](wl::Decorations::Kde* k) {
        if (k != kde_decoration_)
            return;
        kde_decoration_ = nullptr;
        kde_mode_.disconnect();
        kde_destroy_.disconnect();
        refresh_decoration_mode();
    });
    refresh_decoration_mode();
}

bool XdgView::wants_ssd() const {
    return (decoration_ && decoration_->requested != wl::Decorations::ClientSide) ||
           (kde_decoration_ && kde_decoration_->mode == wl::Decorations::ServerSide);
}

void XdgView::apply_decoration_mode() {
    // atrium's title bar unless the client says it draws its own (a
    // browser's tab strip with its buttons): forcing ours on those gave two.
    if (decoration_ && toplevel->base() && toplevel->base()->initialized())
        server.wl->decorations->set_mode(decoration_,
                                         wants_ssd() ? wl::Decorations::ServerSide : wl::Decorations::ClientSide);
    refresh_decoration_mode();
}

// --- popups --------------------------------------------------------------------

void handle_new_xdg_popup(Server& server, wl::Popup* popup) {
    // Lives until the popup's first commit (or its destruction, whichever
    // comes first); after that the scene helpers track the popup themselves.
    struct Watch {
        wl::Connection commit, destroy;
    };
    auto* watch = new Watch;
    wl::Surface* surface = popup->base()->surface();
    watch->destroy = popup->events.destroy.connect([watch] { delete watch; });
    watch->commit = surface->events.commit.connect([&server, popup, surface, watch] {
        if (!popup->base()->initialized())
            return;
        Owner owner = Server::owner_of(surface);
        auto* parent_tree = popup->parent() ? static_cast<scene::Tree*>(popup->parent()->data) : nullptr;
        if (!owner || !parent_tree) {
            delete watch;
            return;
        }
        auto* popup_tree = scene::xdg_surface_create(parent_tree, popup->base());
        surface->data = popup_tree;
        attach_surface_blur(server, popup_tree, surface);

        // Keep the popup on its output, in the toplevel's coordinate space.
        Box box;
        if (owner.layer) {
            if (!owner.layer->output) {
                delete watch;
                popup->dismiss();
                return;
            }
            box = owner.layer->output->box;
            box.x -= owner.layer->tree->x;
            box.y -= owner.layer->tree->y;
        } else {
            View* v = owner.view;
            if (!v->output) {
                delete watch;
                popup->dismiss();
                return;
            }
            box = v->output->usable;
            box.x -= v->geom.x;
            box.y -= v->geom.y + v->top();
        }
        popup->unconstrain_from({box.x, box.y, box.width, box.height});
        delete watch;
    });
}

} // namespace atrium
