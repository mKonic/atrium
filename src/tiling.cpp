// Tiling: a space where windows share the screen instead of floating. Like
// a secret space, it keeps a margin of desktop around the windows (the bar
// and the Dock step aside); one window takes the whole frame, and each new
// one halves the space of the one before it (geometry::dwindle).
#include "geometry.hpp"
#include "output.hpp"
#include "server.hpp"
#include "space.hpp"
#include "view.hpp"

#include <algorithm>

namespace atrium {

bool Server::tileable(const View* v) const {
    return v->mapped && !v->unmanaged() && !v->minimized && !v->fullscreen && !v->maximized && !v->parent() &&
           !v->float_in_tiling &&
           !v->is_dialog() && !v->splash() && !v->passive() && v->space && !v->space->secret;
}

void Server::toggle_tiling(Space* space) {
    if (!space || space->secret || !space->output)
        return;
    space->tiled = !space->tiled;

    // The desktop behind dims and frosts over, as behind a secret space.
    space->ensure_tile_backdrop();
    const bool in = space->tiled;
    const Color dim = config.secret_backdrop;
    const bool blur = config.blur;
    animator.cancel_owner(space, true);
    wlr_scene_node_set_enabled(&space->tile_dim->node, true);
    wlr_scene_node_set_enabled(&space->tile_blur->node, blur);
    animator.start(space, 600, Ease::Standard,
        [space, dim, in](double t) {
            const double a = in ? t : 1 - t;
            Color c = dim;
            c[3] = float(dim[3] * a);
            wlr_scene_rect_set_color(space->tile_dim, premultiplied(c).data());
            wlr_scene_blur_set_alpha(space->tile_blur, float(a));
        },
        [space, in] {
            if (!in) {
                wlr_scene_node_set_enabled(&space->tile_dim->node, false);
                wlr_scene_node_set_enabled(&space->tile_blur->node, false);
            }
        });

    if (space->tiled) {
        // Maximized windows already here join the tiles too.
        for (View* v : views)
            if (v->space == space && v->mapped && v->maximized && !v->fullscreen)
                v->join_tiles();
        retile(space);
    } else {
        for (View* v : views)
            if (v->space == space && v->tiled())
                v->untile();
        space->tile_order.clear();
    }
    spaces_changed();
}

void Server::retile(Space* space) {
    if (!space || !space->tiled || !space->output)
        return;
    std::vector<uint64_t>& order = space->tile_order;
    std::vector<View*> members;
    for (View* v : views)
        if (v->space == space && tileable(v))
            members.push_back(v);
    // Keep the order windows came in; newcomers go last.
    std::erase_if(order, [&](uint64_t id) { return std::ranges::none_of(members, [id](View* v) { return v->id == id; }); });
    std::ranges::sort(members, [](View* a, View* b) { return a->id < b->id; });
    for (View* v : members)
        if (std::ranges::find(order, v->id) == order.end())
            order.push_back(v->id);

    space->ensure_tile_backdrop();  // the screen may have changed size
    const wlr_box frame = geometry::secret_frame(space->output->box, config.secret_margin);
    const std::vector<wlr_box> boxes = geometry::dwindle(order.size(), frame, config.snap_gap);
    for (size_t i = 0; i < order.size(); ++i)
        for (View* v : members)
            if (v->id == order[i])
                v->tile_to(boxes[i]);
}

void Server::tile_drop(View* view, double lx, double ly) {
    Space* space = view->space;
    if (!space || !space->tiled)
        return;
    std::vector<uint64_t>& order = space->tile_order;
    auto self = std::ranges::find(order, view->id);
    for (View* v : views) {
        if (v == view || v->space != space || !v->tiled())
            continue;
        const wlr_box& g = v->geom;
        if (lx >= g.x && lx < g.x + g.width && ly >= g.y && ly < g.y + g.height) {
            auto other = std::ranges::find(order, v->id);
            if (self != order.end() && other != order.end())
                std::iter_swap(self, other);
            break;
        }
    }
    retile(space);  // back into a slot, swapped or not
}

// --- keyboard ------------------------------------------------------------------------

uint32_t Server::direction_from(const std::string& word) {
    if (word == "left") return WLR_EDGE_LEFT;
    if (word == "right") return WLR_EDGE_RIGHT;
    if (word == "up") return WLR_EDGE_TOP;
    if (word == "down") return WLR_EDGE_BOTTOM;
    return WLR_EDGE_NONE;
}

View* Server::neighbor_of(View* from, uint32_t direction) const {
    if (!from || !direction)
        return nullptr;
    std::vector<View*> candidates;
    std::vector<wlr_box> boxes;
    for (View* v : views)
        if (v != from && v->visible() && !v->unmanaged() && !v->hidden_from_lists() &&
            v->space == from->space) {
            candidates.push_back(v);
            boxes.push_back(v->geom);
        }
    const int i = geometry::neighbor(from->geom, boxes, direction);
    return i < 0 ? nullptr : candidates[size_t(i)];
}

void Server::move_direction(View* view, uint32_t direction) {
    if (!direction || view->fullscreen)
        return;
    if (view->tiled() && view->space) {
        View* other = neighbor_of(view, direction);
        if (!other || !other->tiled())
            return;
        std::vector<uint64_t>& order = view->space->tile_order;
        auto a = std::ranges::find(order, view->id), b = std::ranges::find(order, other->id);
        if (a != order.end() && b != order.end())
            std::iter_swap(a, b);
        retile(view->space);
        return;
    }
    // Floating: to that half of the screen, up to all of it, down back to
    // where it was.
    switch (direction) {
    case WLR_EDGE_LEFT:
    case WLR_EDGE_RIGHT: view->snap(direction); break;
    case WLR_EDGE_TOP: view->set_maximized(true); break;
    default:
        if (view->maximized)
            view->set_maximized(false);
        else if (view->snapped)
            view->unsnap(true);
        break;
    }
}

// --- the window's side ---------------------------------------------------------------

void View::tile_to(const wlr_box& box) {
    if (!tiled_) {
        before_tile_ = geom;
        tiled_ = true;
    }
    set_tile_bar_hidden(!server.config.tiled_titlebars);
    request_geometry(box);
}

void View::join_tiles() {
    const wlr_box was = restore;  // back there when tiling ends, not maximized
    set_maximized(false, false);  // retiles
    if (tiled_)
        before_tile_ = was;
}

void View::untile() {
    if (!tiled_)
        return;
    tiled_ = false;
    set_tile_bar_hidden(false);
    const wlr_box area = usable_area();
    wlr_box box;
    if (before_tile_) {
        box = *before_tile_;
    } else {
        // Born tiled: two thirds of the screen, cascaded like a new window.
        std::vector<wlr_box> others;
        for (View* v : server.views)
            if (v != this && v->mapped && !v->minimized && v->space == space && !v->tiled())
                others.push_back(v->geom);
        box = geometry::place(area.width * 2 / 3, area.height * 2 / 3, area, nullptr, others,
                              server.config.cascade_step);
    }
    before_tile_.reset();
    request_geometry(geometry::fit_into(box, area));
}

} // namespace atrium
