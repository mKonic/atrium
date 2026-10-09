#include "view.hpp"

#include "background_effect.hpp"

#include "geometry.hpp"
#include "glass.hpp"
#include "output.hpp"
#include "palette.hpp"
#include "overview.hpp"
#include "switcher.hpp"
#include "seat.hpp"
#include "server.hpp"
#include "session_management.hpp"
#include "snapshot.hpp"
#include "space.hpp"
#include "toplevel_drag.hpp"
#include "wobbly_core.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <vector>

namespace atrium {

namespace {

// caelestia's windowsOut (Hyprland's popin with no percentage): shrinking
// to a point at its centre as it fades, 300 ms, emphasized accelerate.
constexpr int kCloseMs = 300;
// windowsIn: growing from that point, 500 ms, emphasized decelerate.
constexpr int kOpenMs = 500;
// windowsMove: 600 ms on the standard curve (maximize, snap, restore).
constexpr int kMorphMs = 600;
// KWin's Squash and Magic Lamp: 250 ms.
constexpr int kMinimizeMs = 250;

// Behind window content when transparency is off.
constexpr Color kBacking{0.07f, 0.07f, 0.08f, 1.0f};

} // namespace

View::View(Server& srv, Kind k) : server(srv), kind(k), id(srv.next_view_id++) {}

View::~View() {
    forget_icon(*this);
    server.animator.cancel_owner(this, false);
    server.animator.cancel_owner(&ring_, false);
    server.animator.cancel_owner(&glide_dx_, false);
    server.animator.cancel_owner(&reveal_, false);
    server.animator.cancel_owner(&blocked_, false);
    server.animator.cancel_owner(&morph_old_, false);
    server.animator.cancel_owner(&wobbly_, false);
    if (morph_timeout_)
        wl_event_source_remove(morph_timeout_);
    if (morph_idle_)
        wl_event_source_remove(morph_idle_);
    destroy_toplevel_handles();
    std::erase(server.views, this);
}

void View::place_tree() {
    if (tree)
        wlr_scene_node_set_position(&tree->node, geom.x + anim_dx_ + glide_dx_, geom.y + anim_dy_ + glide_dy_);
}

void View::set_anim_offset(int dx, int dy) {
    anim_dx_ = dx;
    anim_dy_ = dy;
    place_tree();
}

void View::set_alpha(float a) {
    alpha_ = a;
    if (!tree || unmanaged())
        return;
    update_decorations();  // shadow, outline, title bar and content all follow alpha_
}

wlr_scene_tree* View::home_tree() const {
    if (fullscreen_front())
        return space ? space->fullscreen_tree : server.layer(Layer::Fullscreen);
    return space ? space->tree : server.layer(Layer::Views);
}

bool View::visible() const {
    return mapped && !minimized && !hidden_by_show_desktop && (!space || space->shown());
}

wlr_box View::usable_area() const {
    // A secret space's windows are as large as maximized ones (under the
    // bar), maximized or not, less any margin asked for.
    if (space && space->secret && output)
        return geometry::secret_frame(output->usable, server.config.secret_margin);
    if (output)
        return output->usable;
    if (server.focused_output)
        return server.focused_output->usable;
    return server.layout_box;
}

// --- map / unmap ---------------------------------------------------------

void View::handle_map() {
    opening_ = true;  // placed where it opens, not glided there
    tree = wlr_scene_tree_create(server.layer(unmanaged() ? Layer::Unmanaged : Layer::Views));
    content = create_content(tree);
    popups = wlr_scene_tree_create(tree);
    surface()->data = popups;  // parent tree for this window's popups
    tree->node.data = content->node.data = popups->node.data = this;
    mapped = true;

    if (unmanaged()) {
        // Menus and tooltips place themselves.
        place_tree();
        if (wants_focus())
            server.focus_view(this);
        return;
    }

    // A new window brings the others back, as in KWin.
    server.set_showing_desktop(false);

    const Config& c = server.config;
    shadow = wlr_scene_shadow_create(tree, 0, 0, c.corner_radius, c.shadow_sigma,
                                     c.shadow_color.data());
    wlr_scene_node_lower_to_bottom(&shadow->node);
    outline = wlr_scene_rect_create(tree, 0, 0, premultiplied(c.outline_color).data());
    outline->accepts_input = false;
    wlr_scene_node_place_above(&outline->node, &shadow->node);
    blur = wlr_scene_blur_create(tree, 0, 0);
    wlr_scene_blur_set_should_only_blur_bottom_layer(blur, true);  // the cheap, shared background blur
    wlr_scene_node_place_above(&blur->node, &outline->node);
    backing = wlr_scene_rect_create(tree, 0, 0, kBacking.data());
    backing->accepts_input = false;
    wlr_scene_node_place_above(&backing->node, &blur->node);

    if (wants_ssd()) {
        titlebar = std::make_unique<Titlebar>(*this, tree);
        geom.height += top();
    }
    layout_frame();

    const RuleResult wish = server.assign_space(this);
    if (space)
        wlr_scene_node_reparent(&tree->node, space->tree);
    float_in_tiling = float_in_tiling || wish.floating.value_or(false);
    keep_above = keep_above || wish.keep_above.value_or(false);
    sticky = sticky || wish.sticky.value_or(false);
    render_unfocused = wish.render_unfocused.value_or(false);
    if (render_unfocused)
        server.render_unfocused_arm();

    std::erase(server.views, this);
    server.views.insert(server.views.begin(), this);
    refresh_blocking();  // a modal dialog greys its parent
    listed_ = true;
    create_toplevel_handles();
    place();
    raise();  // new windows open on top, still under any kept above
    update_decorations();
    server.notify_window(*this, "opened");
    if (wish.fullscreen.value_or(false))
        set_fullscreen(true, true);  // the user's rule
    else if (wish.maximized.value_or(false))
        set_maximized(true);
    else if (const SessionWindow* w = server.sessions ? server.sessions->restoring(this) : nullptr; w && w->fullscreen)
        set_fullscreen(true);
    // A tiled space lays its windows out itself: how the app's window was
    // last left when floating doesn't take it out of the layout.
    else if (remembered_ && remembered_->maximized && !(space && space->tiled))
        set_maximized(true);
    else if (remembered_ && remembered_->snapped && !(space && space->tiled))
        snap(remembered_->snapped);
    remembered_.reset();
    // A window a rule sent to a space you aren't looking at opens quietly,
    // unless the rule says to follow it there (Hyprland's "workspace N" to
    // its "workspace N silent"); so do splash screens, notifications and
    // menus. One that asked for focus as it was launched (with a token from
    // a click or a key) gets it, but a rule's quiet placement holds.
    const bool asked = std::exchange(activate_on_map, false);
    const bool sent_away = (wish.space || !wish.secret.empty()) && space && !space->shown();
    bool focus = (!space || space->shown()) && !splash() && !passive();
    if (sent_away)
        focus = wish.follow.value_or(false);
    else if (asked)
        focus = true;
    if (wish.no_focus.value_or(false))
        focus = false;
    if (focus)
        server.focus_view(this);
    else
        server.spaces_changed();

    // caelestia's windowsIn: Hyprland's popin, growing from a point at its
    // centre as it fades in. The snapshot of its first frame does it, the
    // window itself waiting hidden till it lands.
    if (server.config.animations && tree->node.parent) {
        opening_ = true;
        anim_snap_ = take_snapshot(tree->node.parent);
        wlr_scene_node_set_enabled(&tree->node, false);
        Snapshot* snap = anim_snap_.get();
        const FBox frame = snap->frame_box(), point = popin_box(frame, 0);
        snap->place(point, 0);
        server.animator.start(this, kOpenMs, Ease::EmphasizedDecel, [snap, frame, point](double t) {
            snap->place(lerp(point, frame, t), float(t));
        }, [this] {
            anim_snap_.reset();
            opening_ = false;
            if (tree && !minimized)
                wlr_scene_node_set_enabled(&tree->node, true);
        });
    }
    server.overview->view_mapped(this);
}

void View::handle_unmap() {
    // As it was when it opened: an X11 window can flip override-redirect in
    // between, and a window left listed would linger in the switcher.
    const bool managed = listed_;
    listed_ = false;
    if (managed)
        server.remember_placement(this);
    if (server.overview)
        server.overview->view_unmapped(this);
    if (server.switcher)
        server.switcher->view_unmapped(this);
    server.animator.cancel_owner(this, false);
    server.animator.cancel_owner(&morph_old_, false);
    wobble_stop();
    motion_clear();
    motion_known_ = false;
    // Opening or morphing still: the real tree is what the close starts from.
    anim_snap_.reset();
    morph_old_.reset();
    morph_pending_ = false;
    opening_ = false;
    if (tree)
        wlr_scene_node_set_enabled(&tree->node, !minimized);
    if (managed && visible())
        animate_close();
    alpha_ = 1.0f;
    anim_dx_ = anim_dy_ = 0;
    server.animator.cancel_owner(&glide_dx_, false);
    server.animator.cancel_owner(&reveal_, false);
    glide_dx_ = glide_dy_ = 0;
    opening_ = false;
    server.seat->view_unmapped(this);
    const bool was_focused = server.focused_view == this;
    if (was_focused)
        server.focused_view = nullptr;

    if (managed) {
        server.notify_window(*this, "closed");
        std::erase(server.views, this);
        destroy_toplevel_handles();
    }

    Output* old_output = output;
    const bool was_fullscreen = fullscreen;

    titlebar.reset();
    Space* old_space = space;
    space = nullptr;
    wlr_scene_node_destroy(&tree->node);
    tree = content = popups = nullptr;
    shadow = nullptr;
    outline = nullptr;
    blur = nullptr;
    backing = nullptr;
    not_responding_ = nullptr;
    surface()->data = nullptr;
    mapped = false;
    hidden_by_show_desktop = false;
    refresh_blocking();
    server.animator.cancel_owner(&blocked_, false);
    blocked_ = 0;
    blocked_on_ = false;
    // A window that comes back starts fresh; only its last geometry survives.
    minimized = maximized = fullscreen = covered = activated = activate_on_map = false;
    snapped = 0;
    tile_bar_hidden_ = false;
    resize_edges_ = 0;
    resize_settling_ = false;

    if (was_fullscreen && old_output)
        old_output->refit_views();
    tiled_ = false;
    before_tile_.reset();
    server.retile(old_space);  // the others close the gap
    // The last window of a showing secret space closing puts the space away,
    // as one moving out of it does.
    if (managed && old_space && old_space == server.shown_secret && old_space->empty())
        server.hide_secret();
    // Closed in the space it went fullscreen into: back to where it came from
    // (which prunes that space).
    if (const int home = std::exchange(fullscreen_home, 0);
        home && old_space && old_output && old_output->active == old_space && old_space->empty())
        server.switch_space(old_output, home);
    else
        server.prune_space(old_space);
    if (was_focused || (unmanaged() && wants_focus()))
        server.focus_top();
    server.restack_fullscreen();
    server.seat->refresh_pointer();
}

// Centered on the parent for dialogs, centered on the output otherwise, and
// stepped down-right when that exact spot is already taken, like macOS.
void View::place() {
    View* p = parent();
    Output* target = nullptr;
    if (space && !space->secret)
        target = space->output;
    else if (space && space->secret)
        target = space->output ? space->output : server.focused_output;
    if (p && p->output && p->space == space)
        target = p->output;
    if (!target)
        target = server.focused_output;
    if (!target) {
        double cx = server.seat->cursor->x, cy = server.seat->cursor->y;
        target = server.output_at(cx, cy);
    }
    set_output(target);

    // Torn out of another window (a browser tab): under the pointer, riding
    // the drag that made it.
    if (server.toplevel_drags && server.toplevel_drags->place(this))
        return;

    // A tiled space finds it a slot. Born tiled, it has no floating place
    // to go back to yet (untile() finds it one).
    if (space && space->tiled && server.tileable(this)) {
        tiled_ = true;
        server.retile(space);
        return;
    }

    // A secret space takes the screen, less a margin of blurred desktop.
    if (space && space->secret && !p && !is_dialog()) {
        fit_secret(false);
        return;
    }

    // Splash screens sit in the middle of the screen.
    if (splash() && target) {
        const wlr_box u = usable_area();
        move_to(u.x + (u.width - geom.width) / 2, u.y + (u.height - geom.height) / 2);
        return;
    }

    // Back where the app's window last was, unless the user said where
    // (X11 -geometry); where the app itself asks only counts for a first time.
    if (!remembered_)
        remembered_ = server.placement_for(this);
    bool user = false;
    if (auto want = requested_position(user); want && !p && (user || !remembered_)) {
        const wlr_box frame{want->first, want->second - top(), geom.width, geom.height};
        if (Output* o = server.output_at(frame.x + frame.width / 2.0, frame.y + frame.height / 2.0)) {
            set_output(o);
            move_to(frame.x, frame.y);
            return;
        }
    }
    if (remembered_ && !p) {
        const wlr_box want = geometry::fit_into({target ? target->box.x + remembered_->x : remembered_->x,
                                                 target ? target->box.y + remembered_->y : remembered_->y,
                                                 remembered_->width, remembered_->height}, usable_area());
        if (want.width != geom.width || want.height != geom.height)
            request_geometry(want);
        else
            move_to(want.x, want.y);
        return;
    }

    std::vector<wlr_box> others;
    for (View* v : server.views)
        if (v != this && v->mapped && !v->minimized && v->space == space)
            others.push_back(v->geom);
    const wlr_box* parent_box = (p && p->mapped) ? &p->geom : nullptr;
    const wlr_box g = geometry::place(geom.width, geom.height, usable_area(), parent_box, others,
                                      server.config.cascade_step);

    if (g.width != geom.width || g.height != geom.height)
        request_geometry(g);
    else
        move_to(g.x, g.y);
}

// --- closing ---------------------------------------------------------------------

// The window as drawn now, in a snapshot under `parent` (its frame in
// `parent`'s coordinates), to animate while the real one waits.
std::unique_ptr<Snapshot> View::take_snapshot(wlr_scene_tree* parent) {
    auto snap = std::make_unique<Snapshot>(parent, wlr_box{tree->node.x, tree->node.y, geom.width, geom.height});
    const Config& c = server.config;
    snap->add_shadow(shadow, activated ? c.shadow_color : c.shadow_color_inactive);
    snap->add_rect(outline, activated ? c.outline_color : c.outline_color_inactive);
    snap->add_rect(backing, kBacking);
    // The buffer walk skips disabled trees (a minimized window's).
    const bool was = tree->node.enabled;
    wlr_scene_node_set_enabled(&tree->node, true);
    snap->add_buffers(&tree->node);
    wlr_scene_node_set_enabled(&tree->node, was);
    return snap;
}


// Called while the window's scene is still intact, just before it goes.
void View::animate_close() {
    if (!tree || !server.config.animations)
        return;
    // Straight under the layer, not the space: the space may be pruned while
    // it is still going.
    wlr_scene_tree* layer = server.layer(fullscreen_front() ? Layer::Fullscreen : Layer::Views);
    int lx = 0, ly = 0;
    wlr_scene_node_coords(&tree->node, &lx, &ly);
    Snapshot* snap = take_snapshot(layer).release();
    wlr_scene_node_set_position(&snap->tree()->node, lx - tree->node.x, ly - tree->node.y);
    const FBox frame = snap->frame_box(), point = popin_box(frame, 0);
    snap->place(frame, 1);
    server.animator.start(snap, kCloseMs, Ease::EmphasizedAccel, [snap, frame, point](double t) {
        snap->place(lerp(frame, point, t), float(1 - t));
    }, [snap] { delete snap; });
}

// --- geometry ------------------------------------------------------------------

// The menu bar is a wall: a floating window's top (its title bar) stays
// below it, however the window got there, as on a Mac.
int View::below_bar(int y) const {
    if (fullscreen || unmanaged() || layout_owned() || !output)
        return y;
    return std::max(y, output->usable.y);
}

void View::move_to(int x, int y) {
    geom.x = x;
    geom.y = below_bar(y);
    place_tree();
    notify_position();
    update_output_from_position();
    if (server.sessions)
        server.sessions->view_changed(this);
}

// Placed by atrium (a tile, a snap, full screen, back into a secret frame),
// a window glides there rather than jumps: caelestia's windowsMove, 600 ms
// on the standard curve. Its size lands when the app draws it.
// Maximize, snap, restore morph the window into its new size, as Hyprland's
// windowsMove stretches it and KWin's Maximize crossfades from the old
// picture: the window as it was is kept now, and once the app draws the new
// size both stretch from the old box to the new, the old fading out above.
namespace {

double wobbly_now_ms() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

} // namespace

// The frame in layout coordinates, as the springs see it.
static FBox frame_in_layout(const View& v) {
    int lx = 0, ly = 0;
    wlr_scene_node_coords(&v.tree->node, &lx, &ly);
    return {double(lx), double(ly), double(v.geom.width), double(v.geom.height)};
}

void View::wobble_begin(double px, double py, bool resize) {
    if (!server.config.wobbly || !server.config.animations || !mapped || !tree || !tree->node.enabled ||
        fullscreen || opening_ || morph_pending_ || !tree->node.parent || geom.width <= 0 || geom.height <= 0)
        return;
    if (wobbly_) {
        // Caught again while it settles: held at the new point.
        wobble_stop();
    }
    motion_clear();
    wobbly_ = std::make_unique<Wobbly>(frame_in_layout(*this), wobbly_preset(server.config.wobbliness), FPoint{px, py},
                                       resize);
    wobbly_resize_ = resize;
    wobbly_clock_ = wobbly_now_ms();
    // Open-ended: it ends itself once the window settles.
    server.animator.start(&wobbly_, 1e12, Ease::Linear, [this](double) { wobble_frame(); });
}

void View::wobble_release() {
    if (wobbly_ && tree)
        wobbly_->release(frame_in_layout(*this));
}

void View::wobble_stop() {
    if (!wobbly_)
        return;
    server.animator.cancel_owner(&wobbly_, false);
    wobbly_.reset();
    wobbly_snap_.reset();
    if (tree)
        wlr_scene_node_set_hidden(&tree->node, false);
}

void View::wobble_frame() {
    if (!wobbly_ || !tree)
        return;
    const double now = wobbly_now_ms();
    const double dt = std::min(now - wobbly_clock_, 100.0);  // after a stall, don't fling it
    wobbly_clock_ = now;
    const FBox rect = frame_in_layout(*this);
    if (wobbly_resize_)
        wobbly_->moved(rect);
    if (!wobbly_->advance(rect, dt)) {
        // wobble_stop() cancels this very animation; it's safe from a step.
        wobble_stop();
        return;
    }
    if (!wobbly_->wobbling()) {
        // Still held, nothing bent: the window itself.
        wobbly_snap_.reset();
        wlr_scene_node_set_hidden(&tree->node, false);
        return;
    }
    // A fresh snapshot each frame (the app keeps drawing), just above the
    // window; the frame in its parent's coordinates.
    wobbly_snap_ = take_snapshot(tree->node.parent);
    wlr_scene_node_place_above(&wobbly_snap_->tree()->node, &tree->node);
    wlr_scene_node_set_hidden(&tree->node, true);
    const Wobbly& w = *wobbly_;
    const double ox = rect.x - tree->node.x, oy = rect.y - tree->node.y;  // parent's origin, in layout
    const double W = rect.width, H = rect.height;
    const FBox frame = wobbly_snap_->frame_box();
    wobbly_snap_->warp(
        [&](double x, double y) {
            const FPoint p = w.at(x / W, y / H);
            return FPoint{p.x - ox - frame.x, p.y - oy - frame.y};
        },
        1, W / kWobblyTessellation, H / kWobblyTessellation);
}

void View::motion_clear() {
    if (!motion_snap_)
        return;
    motion_snap_.reset();
    if (tree && !wobbly_)
        wlr_scene_node_set_hidden(&tree->node, false);
}

void View::motion_frame() {
    // 1.0's eight samples.
    constexpr int kSamples = 8;
    if (!server.config.motion_blur || !server.config.animations || !mapped || !tree || !tree->node.enabled ||
        minimized || wobbly_ || anim_snap_ || morph_pending_ || !tree->node.parent) {
        motion_clear();
        motion_known_ = false;
        return;
    }
    const int x = tree->node.x, y = tree->node.y;
    const int back_x = motion_x_ - x, back_y = motion_y_ - y;
    const bool known = std::exchange(motion_known_, true);
    motion_x_ = x;
    motion_y_ = y;
    // Still (or just appeared): drawn as it is, and whatever trail it left
    // goes with the snapshot.
    if (!known || (back_x == 0 && back_y == 0)) {
        motion_clear();
        return;
    }
    motion_snap_ = take_snapshot(tree->node.parent);
    wlr_scene_node_place_above(&motion_snap_->tree()->node, &tree->node);
    wlr_scene_node_set_hidden(&tree->node, true);
    motion_snap_->motion(back_x, back_y, kSamples);
    // A frame after this one, even if it stops here: its trail goes then.
    if (output)
        wlr_output_schedule_frame(output->wlr);
}

void View::begin_morph() {
    wobble_stop();
    motion_clear();
    if (morph_pending_ || !server.config.animations || !mapped || !tree || !tree->node.enabled || fullscreen ||
        opening_ || !tree->node.parent || !visible())
        return;
    server.animator.cancel_owner(&morph_old_, true);
    morph_old_ = take_snapshot(tree->node.parent);
    wlr_scene_node_set_enabled(&morph_old_->tree()->node, false);
    morph_pending_ = true;
    // An app that never draws the size it's given: no morph.
    if (!morph_timeout_)
        morph_timeout_ = wl_event_loop_add_timer(server.loop, [](void* d) {
            auto* self = static_cast<View*>(d);
            if (self->morph_pending_) {
                self->morph_pending_ = false;
                self->morph_old_.reset();
            }
            return 0;
        }, this);
    wl_event_source_timer_update(morph_timeout_, 500);
}

void View::start_morph() {
    morph_pending_ = false;
    wl_event_source_timer_update(morph_timeout_, 0);
    if (!morph_old_ || !tree || !tree->node.parent || !tree->node.enabled)
        return;
    // Straight to where it's going: the morph moves it there.
    server.animator.cancel_owner(&glide_dx_, false);
    glide_dx_ = glide_dy_ = 0;
    place_tree();
    anim_snap_ = take_snapshot(tree->node.parent);
    wlr_scene_node_raise_to_top(&morph_old_->tree()->node);
    wlr_scene_node_set_enabled(&morph_old_->tree()->node, true);
    wlr_scene_node_set_enabled(&tree->node, false);
    Snapshot* before = morph_old_.get();
    Snapshot* after = anim_snap_.get();
    const FBox from = before->frame_box(), to = after->frame_box();
    after->place(from, 1);
    before->place(from, 1);
    server.animator.start(&morph_old_, kMorphMs, Ease::Standard, [before, after, from, to](double t) {
        const FBox box = lerp(from, to, t);
        after->place(box, 1);
        before->place(box, float(1 - t));
    }, [this] {
        morph_old_.reset();
        anim_snap_.reset();
        if (tree && !minimized)
            wlr_scene_node_set_enabled(&tree->node, true);
    });
}

void View::glide_from(int from_x, int from_y) {
    const Seat::Mode m = server.seat->mode;
    if (!mapped || !tree || opening_ || minimized || m == Seat::Mode::Move || m == Seat::Mode::Resize ||
        (from_x == geom.x && from_y == geom.y) || !visible())
        return;
    // From where it is on screen now, a glide in flight included.
    const int dx = from_x + glide_dx_ - geom.x, dy = from_y + glide_dy_ - geom.y;
    server.animator.cancel_owner(&glide_dx_, false);
    server.animator.start(&glide_dx_, 600, Ease::Standard, [this, dx, dy](double t) {
        glide_dx_ = int(std::lround(dx * (1 - t)));
        glide_dy_ = int(std::lround(dy * (1 - t)));
        place_tree();
    });
}

bool View::layout_owned() const {
    return tiled_ || (space && space->secret && !parent() && !is_dialog() && !unmanaged());
}

bool View::fixed_size() const {
    wlr_box min{}, max{};
    size_hints(min, max);
    return max.width > 0 && max.height > 0 && min.width == max.width && min.height == max.height;
}

void View::fit_secret(bool keep_box) {
    if (!space || !space->secret || !output || unmanaged() || parent() || is_dialog() || fullscreen)
        return;
    if (keep_box && !before_secret_)
        before_secret_ = geom;
    // Like a tile: the title bar goes, unless tiles keep theirs.
    set_tile_bar_hidden(!server.config.tiled_titlebars);
    const wlr_box frame = geometry::secret_frame(output->usable, server.config.secret_margin);
    // As large as its hints allow, centered in the frame.
    wlr_box min{}, max{};
    size_hints(min, max);
    const wlr_box inner = geometry::clamp_to_hints(content_box(frame), min, max);
    const int w = inner.width, h = inner.height + top();
    request_geometry({frame.x + (frame.width - w) / 2, frame.y + (frame.height - h) / 2, w, h});
}

void View::leave_secret() {
    if (unmanaged() || parent() || is_dialog())
        return;
    set_tile_bar_hidden(false);
    if (maximized)
        set_maximized(false, false);
    if (fullscreen || snapped)
        return;
    const wlr_box area = usable_area();
    // Born in the secret space: two thirds of the screen, centered.
    wlr_box box = before_secret_.value_or(wlr_box{area.x + area.width / 6, area.y + area.height / 6,
                                                  area.width * 2 / 3, area.height * 2 / 3});
    before_secret_.reset();
    request_geometry(geometry::fit_into(box, area));
}

void View::request_geometry(wlr_box box) {
    // Hints limit the client's content, not the frame around it.
    wlr_box min{}, max{};
    size_hints(min, max);
    const wlr_box inner = geometry::clamp_to_hints(content_box(box), min, max);
    box.width = inner.width;
    box.height = inner.height + top();

    // During an interactive resize the position follows the committed size
    // (see handle_size); moving now would make the window jump ahead of it.
    if (!anchored()) {
        const int from_x = geom.x, from_y = geom.y;
        geom.x = box.x;
        geom.y = box.y = below_bar(box.y);
        glide_from(from_x, from_y);
        place_tree();
        update_output_from_position();
    }
    configure(box);
}

void View::handle_size(int width, int height) {
    height += top();
    if (width == geom.width && height == geom.height)
        return;
    if (anchored() && (resize_edges_ & WLR_EDGE_LEFT))
        geom.x = anchor_right_ - width;
    if (anchored() && (resize_edges_ & WLR_EDGE_TOP))
        geom.y = anchor_bottom_ - height;
    // Resized again while morphing: it lands at once (KWin cancels too).
    if (morph_old_ && !morph_pending_)
        server.animator.cancel_owner(&morph_old_, true);
    geom.width = width;
    geom.height = height;
    place_tree();
    update_decorations();
    // Once every listener has seen this commit (the scene takes the new
    // buffer from it too).
    if (morph_pending_ && !morph_idle_)
        morph_idle_ = wl_event_loop_add_idle(server.loop, [](void* d) {
            auto* self = static_cast<View*>(d);
            self->morph_idle_ = nullptr;
            if (self->morph_pending_)
                self->start_morph();
        }, this);
    // A size that was asked for (a snap, a window command) has landed: the
    // shell hears where the window is now. A drag says so once, as it ends.
    if (!resize_edges_)
        server.notify_window(*this, "changed");
}

void View::begin_resize(uint32_t edges) {
    resize_edges_ = edges;
    resize_settling_ = false;
    anchor_right_ = geom.x + geom.width;
    anchor_bottom_ = geom.y + geom.height;
}

void View::end_resize() {
    if (awaiting_configure()) {
        resize_settling_ = true;
        return;
    }
    settle_resize();
}

void View::settle_resize() {
    resize_edges_ = 0;
    resize_settling_ = false;
    update_output_from_position();
    server.notify_window(*this, "changed");
}

void View::update_output_from_position() {
    Output* o = server.output_at(geom.x + geom.width / 2.0, geom.y + geom.height / 2.0);
    if (!o || o == output)
        return;
    set_output(o);
    // Moved onto another screen: it joins the space showing there, or
    // switching spaces back on the old screen would hide it from this one.
    if (space && !space->secret && o->active && space != o->active)
        server.move_to_space(this, o->active);
}

void View::set_output(Output* o) {
    if (!o || o == output || o->dying)
        return;
    if (handle_ && output)
        wlr_foreign_toplevel_handle_v1_output_leave(handle_, output->wlr);
    output = o;
    if (handle_)
        wlr_foreign_toplevel_handle_v1_output_enter(handle_, output->wlr);
    if (wlr_surface* s = surface()) {
        wlr_fractional_scale_v1_notify_scale(s, o->wlr->scale);
        wlr_surface_set_preferred_buffer_scale(s, int32_t(std::ceil(o->wlr->scale)));
    }
}

int View::top() const {
    return (titlebar && !fullscreen && !tile_bar_hidden_) ? titlebar->height() : 0;
}

// Content and popups sit below the title bar.
void View::layout_frame() {
    if (!tree)
        return;
    wlr_scene_node_set_position(&content->node, 0, top());
    wlr_scene_node_set_position(&popups->node, 0, top());
    if (titlebar) {
        const bool revealed = fullscreen && reveal_ > 0;
        wlr_scene_node_set_enabled(&titlebar->node()->node, top() > 0 || revealed);
        // Over the content, sliding down from under the menu bar.
        const int y = revealed ? reveal_y_ - int(std::lround((1 - reveal_) * Titlebar::kHeight)) : 0;
        wlr_scene_node_set_position(&titlebar->node()->node, 0, y);
        if (revealed)
            wlr_scene_node_raise_to_top(&titlebar->node()->node);
        titlebar->update();
    }
}

void View::reveal_titlebar(bool on, int y) {
    if (!titlebar || (on && !fullscreen))
        return;
    reveal_y_ = y;
    const double from = reveal_;
    const double to = on ? 1.0 : 0.0;
    if (from == to)
        return;
    server.animator.cancel_owner(&reveal_, false);
    server.animator.start(&reveal_, 300, on ? Ease::EmphasizedDecel : Ease::EmphasizedAccel, [this, from, to](double t) {
        reveal_ = from + (to - from) * t;
        layout_frame();
    }, [this, to] {
        reveal_ = to;  // exactly: the curve's end may fall a hair short
        layout_frame();
    });
}

int View::revealed_titlebar_bottom() const {
    return fullscreen && reveal_ > 0 && titlebar ? reveal_y_ + Titlebar::kHeight : 0;
}

void View::refresh_decoration_mode() {
    if (!mapped || unmanaged())
        return;
    const bool want = wants_ssd();
    if (want == bool(titlebar))
        return;
    // The content keeps its size; the frame grows or shrinks by the bar.
    const int old_top = top();
    if (want) {
        titlebar = std::make_unique<Titlebar>(*this, tree);
    } else {
        server.seat->titlebar_gone(titlebar.get());
        titlebar.reset();
    }
    geom.height += top() - old_top;
    layout_frame();
    update_decorations();
    server.seat->refresh_pointer();
}

// --- state -------------------------------------------------------------------

void View::raise() {
    if (!tree)
        return;
    if (keep_below)
        wlr_scene_node_lower_to_bottom(&tree->node);
    else
        wlr_scene_node_raise_to_top(&tree->node);
    // Its dialogs come along, over it.
    for (View* v : server.views)
        if (v != this && v->mapped && v->tree && v->parent() == this)
            v->raise();
    // Windows kept above stay above.
    if (!keep_above)
        for (View* v : server.views)
            if (v != this && v->keep_above && v->mapped && v->tree)
                wlr_scene_node_raise_to_top(&v->tree->node);
}

void View::set_activated(bool a) {
    const bool was = activated;
    activated = a;
    send_activated(a);
    if (handle_)
        wlr_foreign_toplevel_handle_v1_set_activated(handle_, a);
    // The focus ring fades from one tile to the next, as caelestia's border
    // does (600 ms, standard curve).
    server.animator.cancel_owner(&ring_, false);
    if (was != a && layout_owned() && tree) {
        const double from = ring_, to = a ? 1 : 0;
        server.animator.start(&ring_, 600, Ease::Standard, [this, from, to](double t) {
            ring_ = from + (to - from) * t;
            update_decorations();
        });
    } else {
        ring_ = a ? 1 : 0;
    }
    update_decorations();
}

void View::set_maximized(bool m, bool restore_geometry) {
    if (m == maximized || unmanaged())
        return;
    maximized = m;
    send_maximized(m);
    if (handle_)
        wlr_foreign_toplevel_handle_v1_set_maximized(handle_, m);
    server.notify_window(*this, "changed");
    if (fullscreen)
        return;  // takes effect when fullscreen ends
    // In a secret space the frame is the window's place either way, as in
    // caelestia's special workspaces: no title bar, a margin all round.
    if (space && space->secret && !parent() && !is_dialog()) {
        fit_secret(false);
        return;
    }
    if (m) {
        if (!snapped)
            restore = geom;  // a snapped window already remembers where it was
        snapped = 0;
        begin_morph();
        set_tile_bar_hidden(false);
        request_geometry(usable_area());
    } else if (restore_geometry) {
        begin_morph();
        request_geometry(restore);
    }
    server.retile(space);
}

void View::snap(uint32_t zone) {
    if (!zone || unmanaged() || fullscreen || !mapped || (space && space->secret))
        return;
    if (zone == WLR_EDGE_TOP) {
        set_maximized(true);
        return;
    }
    if (maximized)
        set_maximized(false, false);
    else if (!snapped)
        restore = geom;
    begin_morph();
    snapped = zone;
    set_tile_bar_hidden(!server.config.tiled_titlebars);
    request_geometry(geometry::snap_box(usable_area(), zone, server.config.snap_gap));
    server.notify_window(*this, "changed");
}

void View::adopt_size(int width, int height) {
    if (morph_pending_) {
        morph_pending_ = false;
        wl_event_source_timer_update(morph_timeout_, 0);
        morph_old_.reset();
    }
    geom.width = width;
    geom.height = height;
    place_tree();
    update_decorations();
}

void View::unsnap(bool restore_geometry) {
    if (!snapped)
        return;
    snapped = 0;
    if (restore_geometry)
        begin_morph();
    set_tile_bar_hidden(false);
    if (restore_geometry)
        request_geometry(restore);
    server.notify_window(*this, "changed");
}

void View::refresh_tiled_titlebar() {
    if (mapped && space && space->secret) {
        fit_secret(false);
        return;
    }
    if (!snapped || !mapped)
        return;
    set_tile_bar_hidden(!server.config.tiled_titlebars);
    request_geometry(geometry::snap_box(usable_area(), snapped, server.config.snap_gap));
}

void View::set_tile_bar_hidden(bool hidden) {
    if (hidden == tile_bar_hidden_)
        return;
    const int old_top = top();
    tile_bar_hidden_ = hidden;
    geom.height += top() - old_top;
    layout_frame();
    update_decorations();
    server.seat->refresh_pointer();
}

void View::set_fullscreen(bool f, bool by_user) {
    wobble_stop();
    if (f == fullscreen || unmanaged() || !tree)
        return;
    fullscreen_by_user = f && by_user;
    if (f && !maximized)
        restore = geom;  // with its title bar, before it hides
    const int old_top = top();
    if (!f) {
        server.animator.cancel_owner(&reveal_, false);
        reveal_ = 0;
    }
    fullscreen = f;
    geom.height += top() - old_top;  // the bar hides while fullscreen
    layout_frame();
    send_fullscreen(f);
    if (handle_)
        wlr_foreign_toplevel_handle_v1_set_fullscreen(handle_, f);

    covered = false;
    wlr_scene_node_reparent(&tree->node, home_tree());
    if (f) {
        if (output)
            request_geometry(output->box);
    } else {
        request_geometry(maximized ? usable_area() : restore);
    }
    server.restack_fullscreen();
    update_decorations();
    if (output)
        output->refit_views();
    if (!f)
        server.retile(space);
    server.notify_window(*this, "changed");
    server.fullscreen_space(this);
}

bool View::request_minimized(bool m) {
    if (m && fullscreen)
        return minimized;
    set_minimized(m);
    return minimized;
}

void View::set_minimized(bool m) {
    if (m == minimized || unmanaged() || !tree)
        return;
    wobble_stop();
    motion_clear();
    minimized = m;
    server.animator.cancel_owner(this, false);
    server.animator.cancel_owner(&morph_old_, false);
    morph_old_.reset();
    morph_pending_ = false;
    anim_snap_.reset();
    opening_ = false;
    set_alpha(1.0f);
    set_anim_offset(0, 0);
    // Into its Dock icon and back out, from a snapshot (the window itself is
    // hidden meanwhile), 250 ms: KWin's Magic Lamp (Genie on a Mac), on a
    // straight timeline, or its Squash (Scale), cubic. With no icon to go
    // into, it just goes, as in KWin.
    const auto target = server.config.animations ? server.dock_icon_of(*this) : std::nullopt;
    if (target && tree->node.parent && mapped && (!space || space->shown())) {
        anim_snap_ = take_snapshot(tree->node.parent);
        Snapshot* snap = anim_snap_.get();
        // The icon in the snapshot's coordinates (the space's, which may be
        // sliding).
        int px = 0, py = 0;
        wlr_scene_node_coords(&tree->node.parent->node, &px, &py);
        auto local = [px, py](const wlr_box& b) {
            return FBox{double(b.x - px), double(b.y - py), double(b.width), double(b.height)};
        };
        const FBox frame = snap->frame_box();
        const FBox into = local(target->icon);
        wlr_scene_node_set_enabled(&tree->node, false);
        auto restored = [this] {
            anim_snap_.reset();
            if (tree && !minimized)
                wlr_scene_node_set_enabled(&tree->node, true);
        };
        if (server.config.minimize_genie) {
            wlr_box screen{};
            if (output)
                wlr_output_layout_get_box(server.output_layout, output->wlr, &screen);
            const GenieEdge edge = genie_edge(local(screen), local(target->dock), into);
            auto bend = [snap, frame, into, edge](double progress) {
                snap->warp([&](double x, double y) { return genie_point(edge, frame, into, progress, x, y); }, 1);
            };
            bend(m ? 0 : 1);
            if (m)
                server.animator.start(this, kMinimizeMs, Ease::Linear, bend, [this] { anim_snap_.reset(); });
            else
                server.animator.start(this, kMinimizeMs, Ease::Linear, [bend](double t) { bend(1 - t); }, restored);
        } else if (m) {
            snap->place(frame, 1);
            server.animator.start(this, kMinimizeMs, Ease::InCubic, [snap, frame, into](double t) {
                snap->place(lerp(frame, into, t), float(1 - t));
            }, [this] { anim_snap_.reset(); });
        } else {
            snap->place(into, 0);
            server.animator.start(this, kMinimizeMs, Ease::OutCubic, [snap, frame, into](double t) {
                snap->place(lerp(into, frame, t), float(t));
            }, restored);
        }
    } else {
        wlr_scene_node_set_enabled(&tree->node, !m);
    }
    // Kept drawing, it isn't told it's put away either.
    send_suspended(m && !render_unfocused);
    if (handle_)
        wlr_foreign_toplevel_handle_v1_set_minimized(handle_, m);
    server.notify_window(*this, "changed");
    server.restack_fullscreen();
    if (fullscreen && output)
        output->refit_views();

    if (m) {
        if (server.focused_view == this) {
            server.drop_focus();
            server.focus_top();
        }
    } else {
        server.focus_view(this);
    }
    server.seat->refresh_pointer();
    server.retile(space);
}

// --- decorations -------------------------------------------------------------

namespace {

struct RoundCtx {
    int ox, oy;         // the content node's own position, which for_each_buffer adds
    int width, height;  // the content, in content coordinates
    int radius;
    bool round_top;     // false under atrium's title bar, which carries the top corners
    float alpha;
    float saturation, brightness;
};

// Round exactly the buffer corners that sit on a corner of the window. Clients
// build windows out of several surfaces (foot draws its title bar as a
// subsurface, GTK pads the main surface with its own shadow), so no single
// buffer is "the window": what matters is which visible pixels form its corners.
void round_window_corners(wlr_scene_buffer* buffer, int sx, int sy, void* data) {
    auto* ctx = static_cast<RoundCtx*>(data);
    sx -= ctx->ox;
    sy -= ctx->oy;
    if (!wlr_scene_surface_try_from_buffer(buffer))
        return;
    int w = buffer->dst_width, h = buffer->dst_height;
    if ((w <= 0 || h <= 0) && buffer->buffer) {
        w = buffer->buffer->width;
        h = buffer->buffer->height;
    }
    // Visible part after the clip to window geometry.
    const int x0 = std::max(sx, 0), y0 = std::max(sy, 0);
    const int x1 = std::min(sx + w, ctx->width), y1 = std::min(sy + h, ctx->height);
    const int r = ctx->radius;
    const bool left = x0 == 0, right = x1 == ctx->width, top = y0 == 0 && ctx->round_top,
               bottom = y1 == ctx->height;
    wlr_scene_buffer_set_corner_radii(buffer, corner_radii_new(
        top && left ? r : 0, top && right ? r : 0, bottom && right ? r : 0, bottom && left ? r : 0));
    wlr_scene_buffer_set_opacity(buffer, ctx->alpha);
    wlr_scene_buffer_set_tint(buffer, ctx->saturation, ctx->brightness);
}

// dialogparent's saturation 0.4 and brightness 0.6.
float blocked_saturation(double t) {
    return float(1 - 0.6 * t);
}
float blocked_brightness(double t) {
    return float(1 - 0.4 * t);
}

} // namespace

// Rounded corners and a soft shadow on every managed window, stronger on the
// focused one. Nothing while fullscreen.
void View::update_decorations() {
    if (!tree || !shadow || !outline || !blur || !backing || unmanaged())
        return;
    const Config& c = server.config;
    const int radius = fullscreen ? 0 : c.corner_radius;
    // A secret space's backdrop already sets its windows apart; a shadow
    // there only muddies the thin margin of blurred desktop around them.
    const bool show_shadow = c.shadows && !fullscreen && !(space && space->secret);
    wlr_scene_node_set_enabled(&shadow->node, show_shadow);
    if (show_shadow) {
        const float sigma = activated ? c.shadow_sigma : c.shadow_sigma_inactive;
        const int margin = int(std::ceil(sigma));
        wlr_scene_shadow_set_blur_sigma(shadow, sigma);
        Color sc = activated ? c.shadow_color : c.shadow_color_inactive;
        sc[3] *= alpha_;
        wlr_scene_shadow_set_color(shadow, sc.data());
        wlr_scene_shadow_set_corner_radius(shadow, radius);
        wlr_scene_shadow_set_size(shadow, geom.width + 2 * margin, geom.height + 2 * margin);
        wlr_scene_node_set_position(&shadow->node, -margin, -margin);
        // Cut the window's own area out, so a translucent window does not
        // show its shadow through itself.
        wlr_scene_shadow_set_clipped_region(shadow, clipped_region{
            .area = {margin, margin, geom.width, geom.height},
            .corners = corner_radii_all(radius),
        });
    }

    // A faint light hairline keeps dark windows distinct on a dark desktop,
    // where a shadow alone disappears.
    wlr_scene_node_set_enabled(&outline->node, !fullscreen && (c.outline_color[3] > 0 || layout_owned()));
    if (!fullscreen) {
        Color oc = activated ? c.outline_color : c.outline_color_inactive;
        // Tiles and secret windows show which has focus: an accent ring,
        // caelestia's (its primary at 90%, the others faint).
        if (layout_owned()) {
            const uint32_t a = palette::make(c.light, c.accent).accent;
            const Color ring{((a >> 24) & 0xff) / 255.0f, ((a >> 16) & 0xff) / 255.0f, ((a >> 8) & 0xff) / 255.0f, 0.9f};
            const Color faint = c.outline_color_inactive;
            const float t = float(ring_);
            for (int i = 0; i < 4; i++)
                oc[i] = faint[i] + (ring[i] - faint[i]) * t;
        }
        oc[3] *= alpha_;
        wlr_scene_rect_set_color(outline, premultiplied(oc).data());
        wlr_scene_rect_set_size(outline, geom.width + 2, geom.height + 2);
        wlr_scene_node_set_position(&outline->node, -1, -1);
        wlr_scene_rect_set_corner_radius(outline, radius > 0 ? radius + 1 : 0);
        wlr_scene_rect_set_clipped_region(outline, clipped_region{
            .area = {1, 1, geom.width, geom.height},
            .corners = corner_radii_all(radius),
        });
    }

    // atrium's own windows (Settings, the welcome) have Liquid Glass where
    // they say (atrium-glass-v1), as the shell's panels do; they draw
    // everything else themselves.
    const std::vector<GlassShape>* given =
        server.glass_shapes && c.liquid_glass && c.blur ? server.glass_shapes->shapes_for(surface()) : nullptr;
    const bool is_glass = given && !given->empty() && !fullscreen;

    // Translucent content either shows the desktop, frosted, or sits on a
    // solid fill that makes it look opaque.
    wlr_scene_node_set_enabled(&backing->node, !c.transparency && !is_glass);
    if (!c.transparency && !is_glass) {
        Color bc = kBacking;
        bc[3] *= alpha_;
        wlr_scene_rect_set_color(backing, premultiplied(bc).data());
        wlr_scene_rect_set_size(backing, geom.width, geom.height - top());
        wlr_scene_node_set_position(&backing->node, 0, top());
        const int tr = top() ? 0 : radius;
        wlr_scene_rect_set_corner_radii(backing, corner_radii_new(tr, tr, radius, radius));
    }

    // Blur behind translucent windows, or behind just the part an app asked
    // for (ext-background-effect), or none if it asked for none.
    const std::optional<wlr_box> asked =
        server.background_effects ? server.background_effects->blur_for(surface()) : std::nullopt;
    const bool show_blur =
        is_glass || (c.blur && c.transparency && !fullscreen && (!asked || (asked->width > 0 && asked->height > 0)));
    wlr_scene_node_set_enabled(&blur->node, show_blur);
    // Glass sees the windows under it too; plain blur only the desktop.
    wlr_scene_blur_set_should_only_blur_bottom_layer(blur, !is_glass);
    if (!is_glass) {
        wlr_scene_blur_set_strength(blur, 1.0f);
        wlr_scene_blur_set_refraction(blur, 0, 0);
        wlr_scene_blur_set_glass_shapes(blur, nullptr, 0);
    }
    if (is_glass) {
        const int reach = kGlassShadowReach;
        const int w = geom.width, h = geom.height - top();
        wlr_scene_node_set_position(&blur->node, -reach, top() - reach);
        wlr_scene_blur_set_size(blur, w + 2 * reach, h + 2 * reach);
        wlr_scene_blur_set_corner_radius(blur, 0);
        wlr_scene_blur_set_alpha(blur, alpha_);
        apply_glass(blur, *given, float(reach), float(reach), w, h, 1.0, c);
    } else if (show_blur && asked) {
        // In surface coordinates, from the content's corner under the title bar.
        const wlr_box content_area{0, 0, geom.width, geom.height - top()};
        wlr_box b{};
        wlr_box_intersection(&b, &*asked, &content_area);
        wlr_scene_node_set_position(&blur->node, b.x, top() + b.y);
        wlr_scene_blur_set_size(blur, b.width, b.height);
        wlr_scene_blur_set_corner_radius(blur, b.width == geom.width ? radius : 0);
        wlr_scene_blur_set_alpha(blur, alpha_);
    } else if (show_blur) {
        wlr_scene_node_set_position(&blur->node, 0, 0);
        wlr_scene_blur_set_size(blur, geom.width, geom.height);
        wlr_scene_blur_set_corner_radius(blur, radius);
        wlr_scene_blur_set_alpha(blur, alpha_);
    }

    if (titlebar) {
        titlebar->update();
        wlr_scene_buffer_set_opacity(titlebar->node(), alpha_);
        wlr_scene_buffer_set_tint(titlebar->node(), blocked_saturation(blocked_), blocked_brightness(blocked_));
    }
    if (not_responding_) {
        wlr_scene_rect_set_size(not_responding_, geom.width, geom.height);
        wlr_scene_rect_set_corner_radius(not_responding_, radius);
        wlr_scene_node_raise_to_top(&not_responding_->node);
    }
    update_corners();
}

// 300 ms, linear, as dialogparent; from wherever it is, so a dialog closed
// as it opens turns it round.
void View::set_blocked(bool on) {
    if (on == blocked_on_ || !tree)
        return;
    blocked_on_ = on;
    server.animator.cancel_owner(&blocked_, false);
    const double from = blocked_, to = on ? 1 : 0;
    server.animator.start(&blocked_, 300, Ease::Linear, [this, from, to](double t) {
        blocked_ = from + (to - from) * t;
        update_decorations();
    });
}

void View::refresh_blocking() {
    const View* p = mapped && modal() ? parent() : nullptr;
    const uint64_t now = p ? p->id : 0;
    if (now == blocks)
        return;
    blocks = now;
    server.update_blocked();
}

// windowaperture's 250 ms, cubic both ways, from wherever it is now, so a
// change of mind mid-way turns it round.
void View::show_desktop(bool hide, int x, int y) {
    if (hide == hidden_by_show_desktop || !tree || unmanaged())
        return;
    hidden_by_show_desktop = hide;
    wobble_stop();
    motion_clear();
    server.animator.cancel_owner(this, false);
    const int from_dx = anim_dx_, from_dy = anim_dy_;
    const float from_alpha = alpha_;
    const int to_dx = hide ? x - geom.x : 0, to_dy = hide ? y - geom.y : 0;
    const float to_alpha = hide ? 0.0f : 1.0f;
    wlr_scene_node_set_enabled(&tree->node, !minimized);
    server.animator.start(this, 250, Ease::InOutCubic, [=, this](double t) {
        set_anim_offset(from_dx + int(std::lround((to_dx - from_dx) * t)),
                        from_dy + int(std::lround((to_dy - from_dy) * t)));
        set_alpha(from_alpha + float((to_alpha - from_alpha) * t));
    }, [this, hide] {
        // Out of reach too, left past the corner to come back from.
        if (hide && tree)
            wlr_scene_node_set_enabled(&tree->node, false);
    });
}

// Hyprland multiplies the window by 0.8; a black veil at 20% is the same.
void View::set_not_responding(bool on) {
    if (!tree || unmanaged() || on == (not_responding_ != nullptr))
        return;
    if (on) {
        const float veil[4] = {0, 0, 0, 0.2f};
        not_responding_ = wlr_scene_rect_create(tree, geom.width, geom.height, veil);
        not_responding_->accepts_input = false;
        update_decorations();
    } else {
        wlr_scene_node_destroy(&not_responding_->node);
        not_responding_ = nullptr;
    }
}

void View::update_corners() {
    if (!content || unmanaged())
        return;
    RoundCtx ctx{content->node.x, content->node.y, geom.width, geom.height - top(), fullscreen ? 0 : server.config.corner_radius,
                 top() == 0, alpha_, blocked_saturation(blocked_), blocked_brightness(blocked_)};
    wlr_scene_node_for_each_buffer(&content->node, round_window_corners, &ctx);
    if (server.overview)
        server.overview->view_changed(this);
    if (server.switcher)
        server.switcher->view_changed(this);
}

// --- foreign toplevel handles (docks, task switchers, screen sharing) ---------

void View::create_toplevel_handles() {
    wlr_ext_foreign_toplevel_handle_v1_state state{};
    state.title = title();
    state.app_id = app_id();
    ext_handle_ = wlr_ext_foreign_toplevel_handle_v1_create(server.ext_toplevel_list, &state);
    ext_handle_->data = this;

    handle_ = wlr_foreign_toplevel_handle_v1_create(server.toplevel_manager);
    handle_->data = this;
    wlr_foreign_toplevel_handle_v1_set_title(handle_, title());
    wlr_foreign_toplevel_handle_v1_set_app_id(handle_, app_id());
    if (output)
        wlr_foreign_toplevel_handle_v1_output_enter(handle_, output->wlr);
    if (View* p = parent(); p && p->handle_)
        wlr_foreign_toplevel_handle_v1_set_parent(handle_, p->handle_);

    handle_activate_.connect(&handle_->events.request_activate, [this](auto*) {
        if (minimized)
            set_minimized(false);
        server.focus_view(this);
    });
    handle_maximize_.connect(&handle_->events.request_maximize,
        [this](wlr_foreign_toplevel_handle_v1_maximized_event* e) { set_maximized(e->maximized); });
    handle_minimize_.connect(&handle_->events.request_minimize,
        [this](wlr_foreign_toplevel_handle_v1_minimized_event* e) { set_minimized(e->minimized); });
    handle_fullscreen_.connect(&handle_->events.request_fullscreen,
        [this](wlr_foreign_toplevel_handle_v1_fullscreen_event* e) { set_fullscreen(e->fullscreen); });
    handle_close_.connect(&handle_->events.request_close, [this](void*) { close(); });

    // A private scene holding just this window, for per-window screen capture.
    capture_scene_ = wlr_scene_create();
    create_content(&capture_scene_->tree);
}

void View::destroy_toplevel_handles() {
    handle_activate_.disconnect();
    handle_maximize_.disconnect();
    handle_minimize_.disconnect();
    handle_fullscreen_.disconnect();
    handle_close_.disconnect();
    if (handle_) {
        wlr_foreign_toplevel_handle_v1_destroy(handle_);
        handle_ = nullptr;
    }
    if (ext_handle_) {
        wlr_ext_foreign_toplevel_handle_v1_destroy(ext_handle_);
        ext_handle_ = nullptr;
    }
    if (capture_impl_.refresh) {
        wl_event_source_remove(capture_impl_.refresh);
        capture_impl_.refresh = nullptr;
    }
    if (capture_scene_) {
        wlr_scene_node_destroy(&capture_scene_->tree.node);
        capture_scene_ = nullptr;
        capture_source_ = nullptr;  // owned by the scene node
    }
}

void View::update_title() {
    if (ext_handle_) {
        wlr_ext_foreign_toplevel_handle_v1_state state{};
        state.title = title();
        state.app_id = app_id();
        wlr_ext_foreign_toplevel_handle_v1_update_state(ext_handle_, &state);
    }
    if (handle_) {
        wlr_foreign_toplevel_handle_v1_set_title(handle_, title());
        wlr_foreign_toplevel_handle_v1_set_app_id(handle_, app_id());
    }
    if (titlebar)
        titlebar->update();
    if (mapped)
        server.notify_window(*this, "changed");
}

} // namespace atrium
