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
           !v->is_dialog() && !v->splash() && !v->passive() && v->space && !v->space->secret;
}

void Server::toggle_tiling(Space* space) {
    if (!space || space->secret)
        return;
    space->tiled = !space->tiled;
    if (space->tiled) {
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

// --- the window's side ---------------------------------------------------------------

void View::tile_to(const wlr_box& box) {
    if (!tiled_) {
        before_tile_ = geom;
        tiled_ = true;
    }
    set_tile_bar_hidden(!server.config.tiled_titlebars);
    request_geometry(box);
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
