// Server's space management: switching, moving windows between spaces, secret
// spaces, rules at map time, and ext-workspace-v1 for the shell.

#include "ipc.hpp"
#include "output.hpp"
#include "seat.hpp"
#include "server.hpp"
#include "space.hpp"
#include "view.hpp"

#include <algorithm>

namespace atrium {

Space* Server::find_space(Output* output, int number) const {
    for (const auto& s : spaces)
        if (!s->secret && s->output == output && s->number == number)
            return s.get();
    return nullptr;
}

Space* Server::ensure_space(Output* output, int number) {
    if (Space* s = find_space(output, number))
        return s;
    spaces.push_back(std::make_unique<Space>(*this, output, number));
    return spaces.back().get();
}

Space* Server::find_secret(const std::string& name) const {
    for (const auto& s : spaces)
        if (s->secret && s->name == name)
            return s.get();
    return nullptr;
}

Space* Server::ensure_secret(const std::string& name) {
    if (Space* s = find_secret(name))
        return s;
    spaces.push_back(std::make_unique<Space>(*this, name));
    return spaces.back().get();
}

void Server::prune_space(Space* space) {
    if (!space || space->shown() || !space->empty())
        return;
    if (!space->secret && space->output && space->output->active == space)
        return;
    std::erase_if(spaces, [space](const auto& s) { return s.get() == space; });
}

void Server::spaces_changed() {
    if (!ipc)
        return;
    json list = json::array();
    for (const auto& s : spaces)
        list.push_back({{"id", s->id()}, {"label", s->label()}, {"secret", s->secret},
                        {"output", s->output ? s->output->wlr->name : ""}, {"shown", s->shown()}});
    ipc->broadcast("spaces", {{"event", "spaces.changed"}, {"spaces", list}});
}

// Keep a window's position relative to its output when it changes outputs.
static void carry_to_output(View* view, Output* to) {
    if (!to || view->output == to)
        return;
    const wlr_box from = view->output ? view->output->box : to->box;
    view->move_to(view->geom.x - from.x + to->box.x, view->geom.y - from.y + to->box.y);
}

void Server::switch_space(Output* output, int number) {
    if (!output || locked)
        return;
    Space* target = ensure_space(output, number);
    Space* old = output->active;
    if (target == old)
        return;
    if (shown_secret && shown_secret->output == output)
        hide_secret();

    // Finish any switch still in flight on this output first.
    animator.cancel_owner(output, true);

    output->active = target;
    target->set_shown(true);
    focused_output = output;
    if (old && !old->empty()) {
        // Both spaces slide together: toward the left when going to a higher number.
        old->set_shown(false, true);
        const int dir = target->number > old->number ? 1 : -1;
        const int w = output->box.width;
        animator.start(output, 300, Ease::OutQuint, [old, target, dir, w](double t) {
            old->set_offset(int(std::lround(-dir * w * t)), 0);
            target->set_offset(int(std::lround(dir * w * (1 - t))), 0);
        }, [old, target] {
            old->hide_now();
            target->set_offset(0, 0);
        });
    } else if (old) {
        old->set_shown(false);
        prune_space(old);
    }

    output->refit_views();
    focus_view(top_view(output));
    if (!top_view(output))
        focus_view(nullptr);
    seat->refresh_pointer();
    spaces_changed();
}

// Previous/next space on the focused output. "Next" past the last one that
// has windows opens a fresh, empty space, as long as the current one isn't
// already empty.
void Server::step_space(int direction) {
    Output* o = focused_output;
    if (!o || !o->active)
        return;
    const int current = o->active->number;
    if (direction < 0) {
        int best = 0;
        for (const auto& s : spaces)
            if (!s->secret && s->output == o && s->number < current)
                best = std::max(best, s->number);
        if (best)
            switch_space(o, best);
        return;
    }
    int best = 0;
    for (const auto& s : spaces)
        if (!s->secret && s->output == o && s->number > current && (!best || s->number < best))
            best = s->number;
    if (!best && !o->active->empty())
        best = current + 1;
    if (best)
        switch_space(o, best);
}

void Server::move_to_space(View* view, Space* space) {
    if (!view || !space || view->space == space || view->unmanaged())
        return;
    Space* old = view->space;
    const bool was_focused = focused_view == view;

    Output* to = space->secret ? (space->output ? space->output : focused_output) : space->output;
    carry_to_output(view, to);
    view->space = space;
    wlr_scene_node_reparent(&view->tree->node, view->fullscreen ? space->fullscreen_tree : space->tree);

    if (was_focused && !space->shown()) {
        view->set_activated(false);
        focused_view = nullptr;
        focus_top();
    }
    if (old && old->output)
        old->output->refit_views();
    if (view->output)
        view->output->refit_views();
    // The last window leaving a showing secret space takes the overlay with it.
    if (old && old == shown_secret && old->empty()) {
        hide_secret();
        if (was_focused && view->visible())
            focus_view(view);
    }
    prune_space(old);
    seat->refresh_pointer();
    notify_window(*view, "changed");
    spaces_changed();
}

void Server::toggle_secret(const std::string& name) {
    if (locked || !focused_output)
        return;
    if (shown_secret && shown_secret->name == name && shown_secret->output == focused_output) {
        hide_secret();
        return;
    }
    if (shown_secret)
        hide_secret();

    Space* s = ensure_secret(name);
    Output* o = focused_output;
    // Windows come along to the output the space is shown on.
    for (View* v : views)
        if (v->space == s)
            carry_to_output(v, o);
    s->attach(o);
    animator.cancel_owner(s, true);
    s->set_shown(true);
    shown_secret = s;
    fade_secret(s, true);

    View* top = nullptr;
    for (View* v : views)
        if (v->space == s && v->visible()) {
            top = v;
            break;
        }
    if (top)
        focus_view(top);
    else if (focused_view) {
        // Nothing to focus in the secret space: keep the keyboard off the
        // windows under the backdrop.
        focused_view->set_activated(false);
        focused_view = nullptr;
        seat->clear_keyboard_focus();
    }
    seat->refresh_pointer();
    spaces_changed();
}

void Server::hide_secret() {
    Space* s = shown_secret;
    if (!s)
        return;
    shown_secret = nullptr;
    animator.cancel_owner(s, true);
    s->set_shown(false, true);
    fade_secret(s, false);
    if (focused_view && focused_view->space == s) {
        focused_view->set_activated(false);
        focused_view = nullptr;
    }
    focus_top();
    if (!focused_view)
        seat->clear_keyboard_focus();
    seat->refresh_pointer();
    spaces_changed();
}

// The backdrop dims in and the space's windows fade with it; going away is
// the same, backwards, after which the space stops being drawn.
void Server::fade_secret(Space* s, bool in) {
    const Color dim = config.secret_backdrop;
    // By id: a window may close while the space fades.
    std::vector<uint64_t> members;
    for (View* v : views)
        if (v->space == s)
            members.push_back(v->id);
    auto each = [this, members](auto&& fn) {
        for (View* v : views)
            if (std::ranges::find(members, v->id) != members.end())
                fn(v);
    };
    auto step = [s, dim, each, in](double t) {
        const double a = in ? t : 1 - t;
        Color c = dim;
        c[3] = float(dim[3] * a);
        wlr_scene_rect_set_color(s->backdrop, premultiplied(c).data());
        wlr_scene_blur_set_alpha(s->backdrop_blur, float(a));
        s->set_offset(0, int(std::lround((1 - a) * 16)));
        each([a](View* v) { v->set_alpha(float(a)); });
    };
    auto done = [s, each, in] {
        each([](View* v) { v->set_alpha(1.0f); });
        if (!in)
            s->hide_now();
        else
            s->set_offset(0, 0);
    };
    animator.start(s, in ? 220 : 160, in ? Ease::OutQuint : Ease::InCubic, step, done);
}

void Server::reveal(Space* space) {
    if (!space || space->shown())
        return;
    if (space->secret)
        toggle_secret(space->name);
    else
        switch_space(space->output, space->number);
}

RuleResult Server::assign_space(View* view) {
    const RuleResult r = apply_rules(config.rules, view->app_id(), view->title());
    if (!r.secret.empty()) {
        view->space = ensure_secret(r.secret);
    } else if (View* p = view->parent(); p && p->space) {
        view->space = p->space;  // dialogs open where their parent lives
    } else {
        Output* o = focused_output;
        if (!o) {
            for (Output* out : outputs)
                if (out->enabled()) {
                    o = out;
                    break;
                }
        }
        if (o)
            view->space = r.space ? ensure_space(o, r.space) : o->active;
    }
    return r;
}

// --- outputs ----------------------------------------------------------------------------

void Server::output_added(Output* output) {
    output->workspace_group = wlr_ext_workspace_group_handle_v1_create(workspace_manager, 0);
    output->workspace_group->data = output;
    wlr_ext_workspace_group_handle_v1_output_enter(output->workspace_group, output->wlr);
    // Spaces left without an output (the last one went away) join this one.
    for (const auto& s : spaces)
        if (!s->secret && !s->output) {
            s->output = output;
            wlr_ext_workspace_handle_v1_set_group(s->handle, output->workspace_group);
        }
    output->active = ensure_space(output, 1);
    output->active->set_shown(true);
    spaces_changed();
}

// Windows on a departing output's spaces move to the same-numbered spaces of
// another output, keeping their layout.
void Server::output_removing(Output* output) {
    Output* to = nullptr;
    for (Output* o : outputs)
        if (o != output && o->enabled()) {
            to = o;
            break;
        }
    if (shown_secret && shown_secret->output == output)
        hide_secret();

    std::vector<Space*> leaving;
    for (const auto& s : spaces)
        if (s->output == output)
            leaving.push_back(s.get());
    for (Space* s : leaving) {
        if (s->secret) {
            s->output = nullptr;
            continue;
        }
        if (to) {
            Space* target = ensure_space(to, s->number);
            std::vector<View*> moving;
            for (View* v : views)
                if (v->space == s)
                    moving.push_back(v);
            for (View* v : moving)
                move_to_space(v, target);
        }
        if (s->empty() || !to) {
            s->output = nullptr;
            std::erase_if(spaces, [s](const auto& p) { return p.get() == s && p->empty(); });
        }
    }
    output->active = nullptr;
    if (output->workspace_group) {
        wlr_ext_workspace_group_handle_v1_destroy(output->workspace_group);
        output->workspace_group = nullptr;
    }
    spaces_changed();
}

// --- ext-workspace-v1 ------------------------------------------------------------------

void Server::workspace_requests(wlr_ext_workspace_v1_commit_event* event) {
    wlr_ext_workspace_v1_request* req;
    wl_list_for_each(req, event->requests, link) {
        switch (req->type) {
        case WLR_EXT_WORKSPACE_V1_REQUEST_ACTIVATE:
            if (req->activate.workspace && req->activate.workspace->data)
                reveal(static_cast<Space*>(req->activate.workspace->data));
            break;
        case WLR_EXT_WORKSPACE_V1_REQUEST_DEACTIVATE:
            if (req->deactivate.workspace && req->deactivate.workspace->data &&
                static_cast<Space*>(req->deactivate.workspace->data) == shown_secret)
                hide_secret();
            break;
        default:
            break;  // spaces come and go on their own; creation/assignment isn't offered
        }
    }
}

} // namespace atrium
