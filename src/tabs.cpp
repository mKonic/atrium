#include "tabs.hpp"

#include "seat.hpp"
#include "server.hpp"
#include "space.hpp"
#include "titlebar.hpp"
#include "view.hpp"

#include <algorithm>
#include <cstring>

// Window tabs, as a Mac's (Window > Merge All Windows, Show Next Tab, Move
// Tab to New Window; dragging tabs along the bar, out of it and onto
// another window's), kept as Hyprland keeps a group: the windows share the
// frame, the one shown has it, and switching hands the frame and its state
// on (CGroup::setCurrent moves fullscreen over to the new current).

namespace atrium {

namespace {

// An app's own windows: not dialogs, menus, splash screens.
bool tabbable(const View* v) {
    return v && v->mapped && !v->unmanaged() && !v->parent() && !v->is_dialog() && !v->modal() && !v->splash() &&
           !v->passive();
}

bool same_app(const View* a, const View* b) {
    return a->app_id() && b->app_id() && *a->app_id() && std::strcmp(a->app_id(), b->app_id()) == 0;
}

void redraw(const TabGroup* g) {
    if (g)
        for (View* v : g->items)
            if (v->titlebar)
                v->titlebar->update();
}

// How the window handing over goes: behind, out on its own, or closing.
enum class Leaving { Behind, Out, Closing };

} // namespace

// `to` takes the place `from` had: its spot in the stack, its space and how
// it fills it.
static void hand_over(Server& s, View* from, View* to, Leaving how) {
    const bool was_focused = s.focused_view == from;
    if (from->space && to->space != from->space)
        s.move_to_space(to, from->space);
    if (to->minimized)
        to->set_minimized(false);
    to->sticky = from->sticky;
    to->keep_above = from->keep_above;
    to->keep_below = from->keep_below;
    if (from->tree && to->tree && from->tree->node.parent) {
        wlr_scene_node_reparent(&to->tree->node, from->tree->node.parent);
        wlr_scene_node_place_above(&to->tree->node, &from->tree->node);
    }
    if (how == Leaving::Behind && from->tree)
        wlr_scene_node_reparent(&from->tree->node, s.tab_stash);

    const wlr_box frame = from->geom, restore = from->restore;
    // Already there: it doesn't glide in from where it was last shown.
    to->geom.x = frame.x;
    to->geom.y = frame.y;
    if (from->fullscreen) {
        // Fullscreen moves over (and so does the space it took for it).
        if (to->tiled())
            to->untile();
        to->fullscreen_home = std::exchange(from->fullscreen_home, 0);
        to->set_fullscreen(true, from->fullscreen_by_user);
        to->restore = restore;
        if (how != Leaving::Closing)
            from->set_fullscreen(false);
    } else if (from->tiled() && from->space) {
        // Its tile, in its place in the order.
        if (to->fullscreen)
            to->set_fullscreen(false);
        if (to->maximized)
            to->set_maximized(false, false);
        std::vector<uint64_t>& order = from->space->tile_order;
        std::erase(order, to->id);
        std::ranges::replace(order, from->id, to->id);
        to->tile_to(frame);
        if (how != Leaving::Closing)
            s.retile(from->space);  // the window closing goes from the order as it unmaps
    } else {
        if (to->fullscreen)
            to->set_fullscreen(false);
        if (to->tiled())
            to->untile();
        if (from->maximized) {
            to->set_maximized(true);
        } else if (from->snapped) {
            to->snap(from->snapped);
        } else {
            if (to->maximized)
                to->set_maximized(false, false);
            else if (to->snapped)
                to->unsnap(false);
            to->request_geometry(frame);
        }
        to->restore = restore;
    }
    if (was_focused)
        s.focus_view(to);
    s.notify_window(*to, "changed");
    if (how != Leaving::Closing)
        s.notify_window(*from, "changed");
}

void Server::select_tab(View* v) {
    TabGroup* g = v ? v->tabs : nullptr;
    if (!g || g->now() == v)
        return;
    View* old = g->now();
    g->current = *g->index_of(v);
    hand_over(*this, old, v, Leaving::Behind);
    redraw(g);
}

void Server::step_tab(View* v, bool next) {
    if (v && v->tabs)
        select_tab(v->tabs->items[v->tabs->step(next)]);
}

void Server::move_tab(View* v, size_t to) {
    TabGroup* g = v ? v->tabs : nullptr;
    if (!g)
        return;
    g->move(*g->index_of(v), std::min(to, g->size() - 1));
    redraw(g);
}

void Server::merge_tab(View* into, View* v, std::optional<size_t> index) {
    if (!tabbable(into) || !tabbable(v) || into == v)
        return;
    if (into->tabs && into->tabs == v->tabs) {
        if (index)
            move_tab(v, *index > *into->tabs->index_of(v) ? *index - 1 : *index);
        select_tab(v);
        return;
    }
    // Its own tabs come too, each a tab here (Hyprland's CGroup::add).
    if (v->tabs) {
        const std::vector<View*> members = v->tabs->items;
        for (View* m : members)
            if (m->tabs)
                detach_tab(m);
        size_t i = 0;
        for (View* m : members)
            if (m != v)
                merge_tab(into, m, index ? std::optional(*index + i++) : std::nullopt);
        merge_tab(into, v, index ? std::optional(*index + i) : std::nullopt);
        return;
    }

    TabGroup* g = into->tabs;
    if (!g) {
        g = tab_groups.emplace_back(std::make_unique<TabGroup>()).get();
        g->add(into);
        into->tabs = g;
        into->refresh_decoration_mode();  // the tab bar comes
    }
    View* front = g->now();
    if (v->fullscreen)
        v->set_fullscreen(false);  // the tabs' own state is the one that holds
    g->add(v, index);
    v->tabs = g;
    v->refresh_decoration_mode();
    hand_over(*this, front, v, Leaving::Behind);
    redraw(g);
    focus_view(v);
}

// Down to one, it's a window again.
static void dissolve_if_alone(Server& s, TabGroup* g) {
    if (g->size() > 1)
        return;
    for (View* last : g->items)
        last->tabs = nullptr;
    std::erase_if(s.tab_groups, [g](const auto& p) { return p.get() == g; });
}

void Server::detach_tab(View* v, std::optional<wlr_box> frame) {
    TabGroup* g = v ? v->tabs : nullptr;
    if (!g)
        return;
    View* front = g->now();
    const bool layout = front->fullscreen || front->maximized || front->snapped || front->tiled();
    const wlr_box from = layout ? front->restore : front->geom;
    g->remove(v);
    v->tabs = nullptr;
    View* rest = g->now();
    dissolve_if_alone(*this, g);
    rest->refresh_decoration_mode();
    if (front == v) {
        hand_over(*this, v, rest, Leaving::Out);
    } else if (v->tree) {
        // Out of the stash, over the tabs it left.
        wlr_scene_node_reparent(&v->tree->node, v->home_tree());
    }
    v->refresh_decoration_mode();

    // On its own, floating, where asked: else a step down and right, as a
    // new window opens off the one before.
    if (v->fullscreen)
        v->set_fullscreen(false);
    if (v->maximized)
        v->set_maximized(false, false);
    if (v->snapped)
        v->unsnap(false);
    if (v->space && v->space->tiled && tileable(v)) {
        retile(v->space);
    } else {
        wlr_box box = frame.value_or(wlr_box{from.x + 28, from.y + 28, from.width, from.height - Titlebar::kTabHeight});
        box.height = std::max(box.height, 1);
        if (frame) {
            // Torn off under the pointer: there at once, no glide.
            v->geom.x = box.x;
            v->geom.y = box.y;
        }
        v->request_geometry(box);
    }
    redraw(rest->tabs);
    if (rest->titlebar)
        rest->titlebar->update();
    v->raise();
    focus_view(v);
    notify_window(*v, "changed");
    notify_window(*rest, "changed");
}

void Server::tab_closed(View* v) {
    TabGroup* g = v->tabs;
    if (!g)
        return;
    const bool front = g->now() == v;
    g->remove(v);
    v->tabs = nullptr;
    View* next = g->now();
    dissolve_if_alone(*this, g);
    next->refresh_decoration_mode();
    if (front)
        hand_over(*this, v, next, Leaving::Closing);
    redraw(next->tabs);
    if (next->titlebar)
        next->titlebar->update();
    notify_window(*next, "changed");
}

namespace {

// The app's windows on v's space that aren't among its tabs (each one once:
// the window in front of other tabs speaks for them).
std::vector<View*> mergeable(const std::vector<View*>& views, const View* v) {
    std::vector<View*> out;
    if (!tabbable(v))
        return out;
    for (View* w : views)
        if (w != v && (!v->tabs || w->tabs != v->tabs) && tabbable(w) && w->space == v->space && same_app(v, w) &&
            !w->tab_hidden())
            out.push_back(w);
    return out;
}

} // namespace

void Server::merge_all_windows(View* v) {
    if (!tabbable(v))
        return;
    View* shown = v->tabs ? v->tabs->now() : v;
    const std::vector<View*> others = mergeable(views, v);
    // Least recent first, so the newest sit next to the one in front.
    for (auto it = others.rbegin(); it != others.rend(); ++it)
        merge_tab(shown, *it);
    select_tab(shown);
    focus_view(shown);
}

bool Server::open_as_tab(View* v, View* front) {
    if (!front || front == v || !tabbable(v) || !tabbable(front) || front->space != v->space)
        return false;
    if (!tabs::opens_as_tab(config.prefer_tabs, same_app(v, front), front->fullscreen))
        return false;
    merge_tab(front, v);
    return true;
}

} // namespace atrium
