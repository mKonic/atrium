#include "output.hpp"
#include "layer_surface.hpp"
#include "seat.hpp"
#include "server.hpp"
#include "view.hpp"
#include "geometry.hpp"
#include "rules.hpp"

#include <cmath>

namespace atrium {

XdgView::XdgView(Server& srv, wlr_xdg_toplevel* t) : View(srv, Kind::Xdg), toplevel(t) {
    toplevel->base->data = this;
    wlr_surface* s = toplevel->base->surface;

    commit_.connect(&s->events.commit, [this](void*) { commit(); });
    map_.connect(&s->events.map, [this](void*) {
        const wlr_box& g = toplevel->base->geometry;
        geom.width = g.width;
        geom.height = g.height;
        handle_map();
        if (toplevel->requested.fullscreen)
            set_fullscreen(true);
        else if (toplevel->requested.maximized)
            set_maximized(true);
    });
    unmap_.connect(&s->events.unmap, [this](void*) { handle_unmap(); });
    destroy_.connect(&toplevel->events.destroy, [this](void*) { delete this; });

    // xdg-shell wants a configure in reply to every state request, even one
    // the compositor ignores or that changes nothing.
    auto reply = [this] {
        if (toplevel->base->initialized)
            wlr_xdg_surface_schedule_configure(toplevel->base);
    };
    request_fullscreen_.connect(&toplevel->events.request_fullscreen, [this, reply](void*) {
        if (mapped && toplevel->requested.fullscreen != fullscreen)
            set_fullscreen(toplevel->requested.fullscreen);
        else
            reply();
    });
    request_maximize_.connect(&toplevel->events.request_maximize, [this, reply](void*) {
        if (mapped && !fullscreen && toplevel->requested.maximized != maximized)
            set_maximized(toplevel->requested.maximized);
        else
            reply();
    });
    request_minimize_.connect(&toplevel->events.request_minimize, [this](void*) {
        if (mapped)
            set_minimized(true);
    });
    request_move_.connect(&toplevel->events.request_move, [this](wlr_xdg_toplevel_move_event* e) {
        if (mapped && wlr_seat_validate_pointer_grab_serial(server.seat->wlr, surface(), e->serial))
            server.seat->begin_move(this);
    });
    request_resize_.connect(&toplevel->events.request_resize, [this](wlr_xdg_toplevel_resize_event* e) {
        if (mapped && wlr_seat_validate_pointer_grab_serial(server.seat->wlr, surface(), e->serial))
            server.seat->begin_resize(this, e->edges);
    });
    // A right-click on a GTK header bar: atrium's window menu.
    request_window_menu_.connect(&toplevel->events.request_show_window_menu,
        [this](wlr_xdg_toplevel_show_window_menu_event* e) {
            if (!mapped)
                return;
            double ox, oy;
            surface_origin(ox, oy);
            server.show_window_menu(this, ox + e->x, oy + e->y);
        });
    set_title_.connect(&toplevel->events.set_title, [this](void*) { update_title(); });
    set_app_id_.connect(&toplevel->events.set_app_id, [this](void*) { update_title(); });

    // A KDE decoration announced before this toplevel existed.
    wlr_server_decoration* d;
    wl_list_for_each(d, &server.kde_decoration_manager->decorations, link)
        if (d->surface == s) {
            set_kde_decoration(d);
            break;
        }
}

XdgView::~XdgView() {
    toplevel->base->data = nullptr;
}

void XdgView::commit() {
    wlr_xdg_surface* base = toplevel->base;
    if (base->initial_commit) {
        wlr_xdg_toplevel_set_wm_capabilities(toplevel,
            WLR_XDG_TOPLEVEL_WM_CAPABILITIES_MAXIMIZE |
            WLR_XDG_TOPLEVEL_WM_CAPABILITIES_FULLSCREEN |
            WLR_XDG_TOPLEVEL_WM_CAPABILITIES_MINIMIZE);
        if (Output* o = server.focused_output) {
            wlr_fractional_scale_v1_notify_scale(base->surface, o->wlr->scale);
            wlr_surface_set_preferred_buffer_scale(base->surface, int32_t(std::ceil(o->wlr->scale)));
            // Tell the client how much room there is before it picks a size.
            if (wl_resource_get_version(toplevel->resource) >= XDG_TOPLEVEL_CONFIGURE_BOUNDS_SINCE_VERSION)
                wlr_xdg_toplevel_set_bounds(toplevel, o->usable.width,
                                            o->usable.height - (wants_ssd() ? Titlebar::kHeight : 0));
        }
        apply_decoration_mode();
        // A rule sends it to a secret space: start at the size it will have
        // there. Otherwise reopen at the size the app last had, or let it
        // pick its own.
        remembered_ = server.placement_for(this);
        const bool secret = server.focused_output && toplevel->app_id &&
            !apply_rules(server.config.rules, toplevel->app_id, toplevel->title ? toplevel->title : "").secret.empty();
        if (secret && !toplevel->parent) {
            const wlr_box f = geometry::secret_frame(server.focused_output->box, server.config.secret_margin);
            wlr_xdg_toplevel_set_size(toplevel, f.width, std::max(1, f.height - (wants_ssd() ? Titlebar::kHeight : 0)));
        } else if (remembered_)
            wlr_xdg_toplevel_set_size(toplevel, remembered_->width,
                                      std::max(1, remembered_->height - (wants_ssd() ? Titlebar::kHeight : 0)));
        else
            wlr_xdg_toplevel_set_size(toplevel, 0, 0);
        return;
    }
    if (!mapped)
        return;

    // Crop to the window geometry: client-side shadows are the client's
    // business, ours are drawn by the compositor.
    wlr_box clip = base->geometry;
    wlr_scene_subsurface_tree_set_clip(&content->node, &clip);
    handle_size(base->geometry.width, base->geometry.height);
    if (resize_settling_ && !awaiting_configure())
        settle_resize();
    update_corners();
}

bool XdgView::awaiting_configure() const {
    // Serials wrap; compare by signed distance.
    return last_size_serial_ &&
           int32_t(toplevel->base->current.configure_serial - last_size_serial_) < 0;
}

void XdgView::configure(const wlr_box& frame) {
    if (!toplevel->base->initialized)
        return;
    const wlr_box box = content_box(frame);
    if (box.width != toplevel->scheduled.width || box.height != toplevel->scheduled.height)
        last_size_serial_ = wlr_xdg_toplevel_set_size(toplevel, box.width, box.height);
}

wlr_scene_tree* XdgView::create_content(wlr_scene_tree* parent) {
    return wlr_scene_xdg_surface_create(parent, toplevel->base);
}

void XdgView::surface_origin(double& x, double& y) const {
    x = geom.x - toplevel->base->geometry.x;
    y = geom.y + top() - toplevel->base->geometry.y;
}

const char* XdgView::app_id() const {
    return toplevel->app_id ? toplevel->app_id : "";
}

const char* XdgView::title() const {
    return toplevel->title ? toplevel->title : "";
}

View* XdgView::parent() const {
    return toplevel->parent ? static_cast<View*>(toplevel->parent->base->data) : nullptr;
}

void XdgView::size_hints(wlr_box& min, wlr_box& max) const {
    const auto& s = toplevel->current;
    min = {0, 0, s.min_width, s.min_height};
    max = {0, 0, s.max_width, s.max_height};
}

bool XdgView::is_dialog() const {
    wlr_box min, max;
    size_hints(min, max);
    return toplevel->parent ||
           (min.width > 0 && min.height > 0 && (min.width == max.width || min.height == max.height));
}

bool XdgView::modal() const {
    const wlr_xdg_dialog_v1* d = wlr_xdg_dialog_v1_try_from_wlr_xdg_toplevel(toplevel);
    return d && d->modal && toplevel->parent;
}

void XdgView::close() {
    wlr_xdg_toplevel_send_close(toplevel);
}

void XdgView::send_activated(bool a) {
    wlr_xdg_toplevel_set_activated(toplevel, a);
}

void XdgView::send_maximized(bool m) {
    wlr_xdg_toplevel_set_maximized(toplevel, m);
}

void XdgView::send_fullscreen(bool f) {
    wlr_xdg_toplevel_set_fullscreen(toplevel, f);
}

void XdgView::send_suspended(bool s) {
    wlr_xdg_toplevel_set_suspended(toplevel, s);
}

void XdgView::dismiss_popups() {
    wlr_xdg_popup *popup, *tmp;
    wl_list_for_each_safe(popup, tmp, &toplevel->base->popups, link)
        wlr_xdg_popup_destroy(popup);
}

// --- decorations -------------------------------------------------------------

void XdgView::set_decoration(wlr_xdg_toplevel_decoration_v1* d) {
    decoration_ = d;
    decoration_request_.connect(&d->events.request_mode, [this](void*) { apply_decoration_mode(); });
    decoration_destroy_.connect(&d->events.destroy, [this](void*) {
        decoration_ = nullptr;
        decoration_request_.disconnect();
        decoration_destroy_.disconnect();
        refresh_decoration_mode();
    });
    apply_decoration_mode();
}

void XdgView::set_kde_decoration(wlr_server_decoration* d) {
    kde_decoration_ = d;
    kde_mode_.connect(&d->events.mode, [this](void*) { refresh_decoration_mode(); });
    kde_destroy_.connect(&d->events.destroy, [this](void*) {
        kde_decoration_ = nullptr;
        kde_mode_.disconnect();
        kde_destroy_.disconnect();
        refresh_decoration_mode();
    });
    refresh_decoration_mode();
}

bool XdgView::wants_ssd() const {
    return (decoration_ && decoration_->requested_mode != WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE) ||
           (kde_decoration_ && kde_decoration_->mode == WLR_SERVER_DECORATION_MANAGER_MODE_SERVER);
}

void XdgView::apply_decoration_mode() {
    // atrium's title bar unless the client says it draws its own (a
    // browser's tab strip with its buttons): forcing ours on those gave two.
    if (decoration_ && toplevel->base->initialized)
        wlr_xdg_toplevel_decoration_v1_set_mode(decoration_, wants_ssd()
            ? WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE
            : WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE);
    refresh_decoration_mode();
}

// --- popups --------------------------------------------------------------------

void handle_new_xdg_popup(Server&, wlr_xdg_popup* popup) {
    // Lives until the popup's first commit (or its destruction, whichever
    // comes first); after that the scene helpers track the popup themselves.
    struct Watch {
        Listener<> commit;
        Listener<> destroy;
    };
    auto* watch = new Watch;

    watch->destroy.connect(&popup->events.destroy, [watch](void*) { delete watch; });
    watch->commit.connect(&popup->base->surface->events.commit, [popup, watch](void*) {
        if (!popup->base->initial_commit)
            return;

        Owner owner = Server::owner_of(popup->base->surface);
        auto* parent_tree = popup->parent ? static_cast<wlr_scene_tree*>(popup->parent->data) : nullptr;
        if (!owner || !parent_tree) {
            delete watch;
            return;
        }
        popup->base->surface->data = wlr_scene_xdg_surface_create(parent_tree, popup->base);

        // Keep the popup on its output, in the toplevel's coordinate space.
        wlr_box box;
        if (owner.layer) {
            if (!owner.layer->output) {
                delete watch;
                wlr_xdg_popup_destroy(popup);
                return;
            }
            box = owner.layer->output->box;
            box.x -= owner.layer->tree->node.x;
            box.y -= owner.layer->tree->node.y;
        } else {
            View* v = owner.view;
            if (!v->output) {
                delete watch;
                wlr_xdg_popup_destroy(popup);
                return;
            }
            box = v->output->usable;
            box.x -= v->geom.x;
            box.y -= v->geom.y + v->top();
        }
        wlr_xdg_popup_unconstrain_from_box(popup, &box);
        delete watch;
    });
}

} // namespace atrium
