#include "view.hpp"
#include "window_copy.hpp"
#include "warp.hpp"

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
#include "space.hpp"
#include "toplevel_drag.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <vector>

namespace atrium {

namespace {
constexpr double kOpenScale = 0.92;       // a window zooms up from this as it opens
constexpr double kCloseScale = 0.92;      // and down to it as it closes
constexpr double kMinimizedScale = 0.08;  // how small it is on reaching the Dock
constexpr int kDockReach = 40;            // the Dock's middle, above the screen's bottom
constexpr int kDockIcon = 48;             // an icon's size, when the Dock doesn't say
} // namespace

namespace {

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
    end_morph();
    end_wobble();
    destroy_toplevel_handles();
    std::erase(server.views, this);
}

void View::place_tree() {
    if (!tree)
        return;
    // Scaled about the frame's centre: the origin moves in by what it loses.
    const int cx = int(std::lround((1 - anim_scale_) * geom.width / 2.0));
    const int cy = int(std::lround((1 - anim_scale_) * geom.height / 2.0));
    tree->set_scale(anim_scale_);
    tree->set_position(geom.x + anim_dx_ + glide_dx_ + cx, geom.y + anim_dy_ + glide_dy_ + cy);
}

void View::set_anim_scale(float scale) {
    anim_scale_ = scale;
    place_tree();
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

scene::Tree* View::home_tree() const {
    if (fullscreen_front())
        return space ? space->fullscreen_tree : server.layer(Layer::Fullscreen);
    return space ? space->tree : server.layer(Layer::Views);
}

bool View::visible() const {
    return mapped && !minimized && (!space || space->shown());
}

Box View::usable_area() const {
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
    tree = scene::Tree::create(server.layer(unmanaged() ? Layer::Unmanaged : Layer::Views));
    content = create_content(tree);
    popups = scene::Tree::create(tree);
    surface()->data = popups;  // parent tree for this window's popups
    tree->data = content->data = popups->data = this;
    mapped = true;

    if (unmanaged()) {
        // Menus and tooltips place themselves.
        place_tree();
        if (wants_focus())
            server.focus_view(this);
        return;
    }

    const Config& c = server.config;
    shadow = scene::Shadow::create(tree, 0, 0, c.corner_radius, c.shadow_sigma, c.shadow_color.data());
    shadow->lower_to_bottom();
    outline = scene::Rect::create(tree, 0, 0, premultiplied(c.outline_color).data());
    outline->accepts_input = false;
    outline->place_above(shadow);
    tree->set_motion_blur(true);  // when windows.motion_blur is on
    blur = scene::Blur::create(tree, 0, 0);
    blur->set_use_cache(true);  // the cheap, shared background blur
    blur->place_above(outline);
    backing = scene::Rect::create(tree, 0, 0, kBacking.data());
    backing->accepts_input = false;
    backing->place_above(blur);

    if (wants_ssd()) {
        titlebar = std::make_unique<Titlebar>(*this, tree);
        geom.height += top();
    }
    layout_frame();

    const RuleResult wish = server.assign_space(this);
    if (space)
        tree->reparent(space->tree);
    float_in_tiling = float_in_tiling || wish.floating.value_or(false);
    keep_above = keep_above || wish.keep_above.value_or(false);
    sticky = sticky || wish.sticky.value_or(false);

    std::erase(server.views, this);
    server.views.insert(server.views.begin(), this);
    listed_ = true;
    create_toplevel_handles();
    place();
    raise();  // new windows open on top, still under any kept above
    update_decorations();
    server.notify_window(*this, "opened");
    if (wish.fullscreen.value_or(false))
        set_fullscreen(true);
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

    // Fade in, zooming up from a little smaller (macOS: 500 ms, emphasized
    // decelerate).
    opening_ = true;
    server.animator.start(this, 500, Ease::EmphasizedDecel, [this](double t) {
        set_alpha(float(t));
        set_anim_scale(float(kOpenScale + (1 - kOpenScale) * t));
    }, [this] {
        opening_ = false;
        set_anim_scale(1.0f);
    });
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
    end_morph();
    end_wobble();
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
    tree->destroy();
    tree = content = popups = nullptr;
    shadow = nullptr;
    outline = nullptr;
    blur = nullptr;
    backing = nullptr;
    surface()->data = nullptr;
    mapped = false;
    // A window that comes back starts fresh; only its last geometry survives.
    minimized = maximized = fullscreen = covered = activated = activate_on_map = false;
    anim_scale_ = 1.0f;
    anim_dx_ = anim_dy_ = 0;
    snapped = 0;
    tile_bar_hidden_ = false;
    resize_edges_ = 0;
    resize_settling_ = false;

    if (was_fullscreen && old_output)
        old_output->refit_views();
    tiled_ = false;
    before_tile_.reset();
    server.retile(old_space);  // the others close the gap
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
        const Box u = usable_area();
        move_to(u.x + (u.width - geom.width) / 2, u.y + (u.height - geom.height) / 2);
        return;
    }

    // Back where the app's window last was, unless the user said where
    // (X11 -geometry); where the app itself asks only counts for a first time.
    if (!remembered_)
        remembered_ = server.placement_for(this);
    bool user = false;
    if (auto want = requested_position(user); want && !p && (user || !remembered_)) {
        const Box frame{want->first, want->second - top(), geom.width, geom.height};
        if (Output* o = server.output_at(frame.x + frame.width / 2.0, frame.y + frame.height / 2.0)) {
            set_output(o);
            move_to(frame.x, frame.y);
            return;
        }
    }
    if (remembered_ && !p) {
        const Box want = geometry::fit_into({target ? target->box.x + remembered_->x : remembered_->x,
                                                 target ? target->box.y + remembered_->y : remembered_->y,
                                                 remembered_->width, remembered_->height}, usable_area());
        if (want.width != geom.width || want.height != geom.height)
            request_geometry(want);
        else
            move_to(want.x, want.y);
        return;
    }

    std::vector<Box> others;
    for (View* v : server.views)
        if (v != this && v->mapped && !v->minimized && v->space == space)
            others.push_back(v->geom);
    const Box* parent_box = (p && p->mapped) ? &p->geom : nullptr;
    const Box g = geometry::place(geom.width, geom.height, usable_area(), parent_box, others,
                                      server.config.cascade_step);

    if (g.width != geom.width || g.height != geom.height)
        request_geometry(g);
    else
        move_to(g.x, g.y);
}

// --- closing ---------------------------------------------------------------------

namespace {

// What stays on screen for a moment after a window is gone: copies of its last
// buffers, shadow and outline, fading out on their own.
struct Ghost {
    scene::Tree* tree = nullptr;
    int origin_x = 0, origin_y = 0;  // for_each_buffer counts the root's own position
    std::vector<scene::Buffer*> buffers;
    std::vector<float> opacity;  // each buffer's own opacity at close
    scene::Shadow* shadow = nullptr;
    scene::Rect* outline = nullptr;
    scene::Rect* backing = nullptr;
    Color shadow_color{}, outline_color{};
    int x = 0, y = 0;
    int w = 0, h = 0;  // the frame, which it shrinks about the centre of
};

void copy_into_ghost(scene::Buffer* src, int sx, int sy, void* data) {
    auto* g = static_cast<Ghost*>(data);
    if (!src->buffer)
        return;
    scene::Buffer* dst = scene::Buffer::create(g->tree, src->buffer);
    dst->set_position(sx - g->origin_x, sy - g->origin_y);
    dst->set_source_box(&src->src_box);
    dst->set_dest_size(src->dst_width, src->dst_height);
    dst->set_transform(src->transform);
    dst->set_corner_radii(src->corners);
    g->buffers.push_back(dst);
    g->opacity.push_back(src->opacity);
}

} // namespace

// Called while the window's scene is still intact, just before it goes.
void View::animate_close() {
    if (!tree || !server.config.animations)
        return;
    auto* g = new Ghost;
    // Straight under the layer, not the space: the space may be pruned while
    // the ghost is still fading.
    g->tree = scene::Tree::create(server.layer(fullscreen_front() ? Layer::Fullscreen : Layer::Views));
    g->x = tree->x;
    g->y = tree->y;
    g->w = geom.width;
    g->h = geom.height;
    g->tree->set_position(g->x, g->y);

    const Config& c = server.config;
    if (shadow && shadow->enabled) {
        g->shadow_color = activated ? c.shadow_color : c.shadow_color_inactive;
        g->shadow = scene::Shadow::create(g->tree, shadow->width, shadow->height, shadow->corner_radius, shadow->blur_sigma, g->shadow_color.data());
        g->shadow->set_position(shadow->x, shadow->y);
        g->shadow->set_cut_out(shadow->cut);
    }
    if (outline && outline->enabled) {
        g->outline_color = activated ? c.outline_color : c.outline_color_inactive;
        g->outline = scene::Rect::create(g->tree, outline->width, outline->height, premultiplied(g->outline_color).data());
        g->outline->accepts_input = false;
        g->outline->set_position(outline->x, outline->y);
        g->outline->set_corner_radii(outline->corners);
        g->outline->set_cut_out(outline->cut);
    }
    if (backing && backing->enabled) {
        g->backing = scene::Rect::create(g->tree, backing->width, backing->height, premultiplied(kBacking).data());
        g->backing->set_position(backing->x, backing->y);
        g->backing->set_corner_radii(backing->corners);
    }
    g->origin_x = tree->x;
    g->origin_y = tree->y;
    tree->for_each_buffer([&](scene::Buffer* b_, int x_, int y_) { (copy_into_ghost)(b_, x_, y_, g); });

    server.animator.start(g, 300, Ease::EmphasizedAccel, [g](double t) {
        const float a = float(1 - t);
        for (size_t i = 0; i < g->buffers.size(); ++i)
            g->buffers[i]->set_opacity(g->opacity[i] * a);
        if (g->shadow) {
            Color sc = g->shadow_color;
            sc[3] *= a;
            g->shadow->set_color(sc.data());
        }
        if (g->outline) {
            Color oc = g->outline_color;
            oc[3] *= a;
            g->outline->set_color(premultiplied(oc).data());
        }
        if (g->backing) {
            Color bc = kBacking;
            bc[3] *= a;
            g->backing->set_color(premultiplied(bc).data());
        }
        const float sc = float(1 - (1 - kCloseScale) * t);
        g->tree->set_scale(sc);
        g->tree->set_position(g->x + int(std::lround((1 - sc) * g->w / 2.0)),
                              g->y + int(std::lround((1 - sc) * g->h / 2.0)));
    }, [g] {
        g->tree->destroy();
        delete g;
    });
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
    Box min{}, max{};
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
    const Box frame = geometry::secret_frame(output->usable, server.config.secret_margin);
    // As large as its hints allow, centered in the frame.
    Box min{}, max{};
    size_hints(min, max);
    const Box inner = geometry::clamp_to_hints(content_box(frame), min, max);
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
    const Box area = usable_area();
    // Born in the secret space: two thirds of the screen, centered.
    Box box = before_secret_.value_or(Box{area.x + area.width / 6, area.y + area.height / 6,
                                                  area.width * 2 / 3, area.height * 2 / 3});
    before_secret_.reset();
    request_geometry(geometry::fit_into(box, area));
}

void View::request_geometry(Box box) {
    // Hints limit the client's content, not the frame around it.
    Box min{}, max{};
    size_hints(min, max);
    const Box inner = geometry::clamp_to_hints(content_box(box), min, max);
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
    requested_ = box;
    configure(box);
}

void View::handle_size(int width, int height) {
    height += top();
    if (width == geom.width && height == geom.height)
        return;
    if (anchored() && (resize_edges_ & EDGE_LEFT))
        geom.x = anchor_right_ - width;
    if (anchored() && (resize_edges_ & EDGE_TOP))
        geom.y = anchor_bottom_ - height;
    geom.width = width;
    geom.height = height;
    place_tree();
    update_decorations();
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
    output = o;
    sync_handle();
    if (wl::Surface* s = surface()) {
        server.wl->fractional_scales->set_preferred_scale(s, o->screen->scale);
        s->set_preferred_scale(int32_t(std::ceil(o->screen->scale)));
    }
}

int View::top() const {
    return (titlebar && !fullscreen && !tile_bar_hidden_) ? titlebar->height() : 0;
}

// Content and popups sit below the title bar.
void View::layout_frame() {
    if (!tree)
        return;
    content->set_position(0, top());
    popups->set_position(0, top());
    if (titlebar) {
        const bool revealed = fullscreen && reveal_ > 0;
        titlebar->node()->set_enabled(top() > 0 || revealed);
        // Over the content, sliding down from under the menu bar.
        const int y = revealed ? reveal_y_ - int(std::lround((1 - reveal_) * Titlebar::kHeight)) : 0;
        titlebar->node()->set_position(0, y);
        if (revealed)
            titlebar->node()->raise_to_top();
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
        tree->lower_to_bottom();
    else
        tree->raise_to_top();
    // Its dialogs come along, over it.
    for (View* v : server.views)
        if (v != this && v->mapped && v->tree && v->parent() == this)
            v->raise();
    // Windows kept above stay above.
    if (!keep_above)
        for (View* v : server.views)
            if (v != this && v->keep_above && v->mapped && v->tree)
                v->tree->raise_to_top();
}

void View::set_activated(bool a) {
    const bool was = activated;
    activated = a;
    send_activated(a);
    sync_handle();
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
    sync_handle();
    server.notify_window(*this, "changed");
    if (fullscreen)
        return;  // takes effect when fullscreen ends
    // In a secret space the frame is the window's place either way, as in
    // caelestia's special workspaces: no title bar, a margin all round.
    if (space && space->secret && !parent() && !is_dialog()) {
        fit_secret(false);
        return;
    }
    const Box from = geom;
    if (m) {
        if (!snapped)
            restore = geom;  // a snapped window already remembers where it was
        snapped = 0;
        set_tile_bar_hidden(false);
        request_geometry(usable_area());
    } else if (restore_geometry) {
        request_geometry(restore);
    }
    morph_from(from);
    server.retile(space);
}

void View::snap(uint32_t zone) {
    if (!zone || unmanaged() || fullscreen || !mapped || (space && space->secret))
        return;
    if (zone == EDGE_TOP) {
        set_maximized(true);
        return;
    }
    if (maximized)
        set_maximized(false, false);
    else if (!snapped)
        restore = geom;
    const Box from = geom;
    snapped = zone;
    set_tile_bar_hidden(!server.config.tiled_titlebars);
    request_geometry(geometry::snap_box(usable_area(), zone, server.config.snap_gap));
    morph_from(from);
    server.notify_window(*this, "changed");
}

void View::unsnap(bool restore_geometry) {
    if (!snapped)
        return;
    const Box from = geom;
    snapped = 0;
    set_tile_bar_hidden(false);
    if (restore_geometry) {
        request_geometry(restore);
        morph_from(from);
    }
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

void View::morph_from(const Box& from) {
    // The size comes when the app draws it; where it goes is known now.
    const Box to{geom.x, geom.y, requested_.width, requested_.height};
    if (!server.config.animations || !tree || !tree->parent || !mapped || opening_ ||
        (from.width == to.width && from.height == to.height))
        return;
    end_morph();
    // The window itself goes straight to its new place, hidden: the copy
    // makes the whole move, and the two cross-fade at the end.
    server.animator.cancel_owner(&glide_dx_, false);
    glide_dx_ = glide_dy_ = 0;
    place_tree();
    // What it showed at its old size, just over it.
    morph_ = std::make_unique<WindowCopy>(*this, tree->parent);
    scene::Tree* copy = morph_->tree();
    copy->place_above(tree);
    copy->set_position(from.x, from.y);
    morph_->place(from.width, from.height);
    server.animator.start(&morph_, 350, Ease::Standard, [this, from, to](double t) {
        if (!morph_)
            return;
        const auto lerp = [t](int a, int b) { return int(std::lround(a + (b - a) * t)); };
        morph_->tree()->set_position(lerp(from.x, to.x), lerp(from.y, to.y));
        morph_->place(std::max(1, lerp(from.width, to.width)), std::max(1, lerp(from.height, to.height)));
        // The window comes in under the copy before the copy goes, so what's
        // behind never shows through the two.
        set_alpha(float(std::clamp((t - 0.55) / 0.2, 0.0, 1.0)));
        morph_->tree()->set_opacity(1 - float(std::clamp((t - 0.75) / 0.25, 0.0, 1.0)));
    }, [this] {
        morph_.reset();
        set_alpha(1.0f);
    });
}

void View::end_morph() {
    server.animator.cancel_owner(&morph_, false);
    if (morph_) {
        morph_.reset();
        alpha_ = 1.0f;
    }
}

struct WobbleState {
    warp::Wobble springs;
    int64_t last_ns = 0;
};

namespace {

int64_t mono_ns() {
    timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return int64_t(t.tv_sec) * 1000000000 + t.tv_nsec;
}

} // namespace

void View::wobble(int dx, int dy, double hx, double hy) {
    if (!server.config.wobbly || !server.config.animations || !tree || (!dx && !dy))
        return;
    const bool running = wobble_ != nullptr;
    if (!wobble_) {
        wobble_ = std::make_unique<WobbleState>();
        wobble_->last_ns = mono_ns();
    }
    const double gu = geom.width > 0 ? (hx - geom.x) / geom.width : 0.5;
    const double gv = geom.height > 0 ? (hy - geom.y) / geom.height : 0.5;
    wobble_->springs.moved(dx, dy, gu, gv);
    if (running)
        return;
    // Every frame until it settles: the springs move on by real time, and
    // the frame is drawn through them.
    struct Tick {
        static void start(View* v) {
            v->server.animator.start(&v->wobble_, 1000, Ease::Linear, [v](double) {
                if (!v->wobble_ || !v->tree)
                    return;
                const int64_t now = mono_ns();
                v->wobble_->springs.advance(double(now - v->wobble_->last_ns) / 1e9);
                v->wobble_->last_ns = now;
                const FBox frame{double(v->geom.x), double(v->geom.y), double(v->geom.width), double(v->geom.height)};
                const warp::Wobble* springs = &v->wobble_->springs;
                v->tree->set_warp([frame, springs](double u, double w) {
                    auto [ox, oy] = springs->offset(u, w);
                    return std::pair{frame.x + u * frame.width + ox, frame.y + w * frame.height + oy};
                }, frame);
            }, [v] {
                if (!v->wobble_)
                    return;
                if (v->wobble_->springs.stable() && v->server.seat->mode != Seat::Mode::Move)
                    v->end_wobble();
                else
                    start(v);
            });
        }
    };
    Tick::start(this);
}

void View::end_wobble() {
    server.animator.cancel_owner(&wobble_, false);
    if (wobble_ && tree)
        tree->set_warp({}, {});
    wobble_.reset();
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

void View::set_fullscreen(bool f) {
    if (f == fullscreen || unmanaged() || !tree)
        return;
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
    sync_handle();

    covered = false;
    tree->reparent(home_tree());
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
    minimized = m;
    // Into the Dock or back out of it; the tree stays enabled while it
    // animates out.
    server.animator.cancel_owner(this, false);
    tree->set_enabled(true);
    // Into its own Dock icon when the Dock shows one, else the Dock's middle.
    Box icon{0, 0, kDockIcon, kDockIcon};
    if (output) {
        icon = server.dock_icon_of(*this).value_or(Box{output->box.x + output->box.width / 2 - kDockIcon / 2,
                                                            output->box.y + output->box.height - kDockReach - kDockIcon / 2,
                                                            kDockIcon, kDockIcon});
    }
    const double to_x = icon.x + icon.width / 2.0, to_y = icon.y + icon.height / 2.0;
    const auto settle = [this] {
        if (minimized && tree)
            tree->set_enabled(false);
        if (tree)
            tree->set_warp({}, {});
        set_alpha(1.0f);
        set_anim_offset(0, 0);
        set_anim_scale(1.0f);
    };
    if (server.config.minimize_genie && output) {
        // macOS's Genie: poured into the icon, and back out of it.
        const FBox frame{double(geom.x), double(geom.y), double(geom.width), double(geom.height)};
        const double icon_w = icon.width;
        const auto step = [this, frame, to_x, to_y, icon_w](double k) {  // 0: in place, 1: in the Dock
            if (!tree)
                return;
            tree->set_warp([frame, to_x, to_y, icon_w, k](double u, double v) {
                return warp::genie(frame.x, frame.y, frame.width, frame.height, to_x, to_y, icon_w, k, u, v);
            }, frame);
            set_alpha(float(std::clamp((1 - k) * 8, 0.0, 1.0)));  // gone as it arrives
        };
        if (m)
            server.animator.start(this, 500, Ease::EmphasizedAccel, [step](double t) { step(t); }, settle);
        else
            server.animator.start(this, 500, Ease::EmphasizedDecel, [step](double t) { step(1 - t); }, settle);
    } else {
        // macOS's Scale: shrunk into the icon, and grown back out.
        const int dx = int(std::lround(to_x - (geom.x + geom.width / 2.0)));
        const int dy = int(std::lround(to_y - (geom.y + geom.height / 2.0)));
        const auto step = [this, dx, dy](double k) {
            set_anim_scale(float(1 - (1 - kMinimizedScale) * k));
            set_anim_offset(int(std::lround(dx * k)), int(std::lround(dy * k)));
            set_alpha(float(std::clamp(1.4 - 1.4 * k, 0.0, 1.0)));
        };
        if (m)
            server.animator.start(this, 400, Ease::EmphasizedAccel, [step](double t) { step(t); }, settle);
        else
            server.animator.start(this, 500, Ease::EmphasizedDecel, [step](double t) { step(1 - t); }, settle);
    }
    send_suspended(m);
    sync_handle();
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
};

// Round exactly the buffer corners that sit on a corner of the window. Clients
// build windows out of several surfaces (foot draws its title bar as a
// subsurface, GTK pads the main surface with its own shadow), so no single
// buffer is "the window": what matters is which visible pixels form its corners.
void round_window_corners(scene::Buffer* buffer, int sx, int sy, void* data) {
    auto* ctx = static_cast<RoundCtx*>(data);
    sx -= ctx->ox;
    sy -= ctx->oy;
    if (!buffer->surface())
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
    buffer->set_corner_radii(scene::Radii(top && left ? r : 0, top && right ? r : 0, bottom && right ? r : 0, bottom && left ? r : 0));
    buffer->set_opacity(ctx->alpha);
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
    shadow->set_enabled(show_shadow);
    if (show_shadow) {
        const float sigma = activated ? c.shadow_sigma : c.shadow_sigma_inactive;
        const int margin = int(std::ceil(sigma));
        shadow->set_blur_sigma(sigma);
        Color sc = activated ? c.shadow_color : c.shadow_color_inactive;
        sc[3] *= alpha_;
        shadow->set_color(sc.data());
        shadow->set_corner_radius(radius);
        shadow->set_size(geom.width + 2 * margin, geom.height + 2 * margin);
        shadow->set_position(-margin, -margin);
        // Cut the window's own area out, so a translucent window does not
        // show its shadow through itself.
        shadow->set_cut_out(scene::CutOut{
            .area = {margin, margin, geom.width, geom.height},
            .corners = scene::Radii::all(radius),
        });
    }

    // A faint light hairline keeps dark windows distinct on a dark desktop,
    // where a shadow alone disappears.
    outline->set_enabled(!fullscreen && (c.outline_color[3] > 0 || layout_owned()));
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
        outline->set_color(premultiplied(oc).data());
        outline->set_size(geom.width + 2, geom.height + 2);
        outline->set_position(-1, -1);
        outline->set_corner_radius(radius > 0 ? radius + 1 : 0);
        outline->set_cut_out(scene::CutOut{
            .area = {1, 1, geom.width, geom.height},
            .corners = scene::Radii::all(radius),
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
    backing->set_enabled(!c.transparency && !is_glass);
    if (!c.transparency && !is_glass) {
        Color bc = kBacking;
        bc[3] *= alpha_;
        backing->set_color(premultiplied(bc).data());
        backing->set_size(geom.width, geom.height - top());
        backing->set_position(0, top());
        const int tr = top() ? 0 : radius;
        backing->set_corner_radii(scene::Radii(tr, tr, radius, radius));
    }

    // Blur behind translucent windows, or behind just the part an app asked
    // for (ext-background-effect), or none if it asked for none.
    const std::optional<Box> asked =
        server.background_effects ? server.background_effects->blur_for(surface()) : std::nullopt;
    const bool show_blur =
        is_glass || (c.blur && c.transparency && !fullscreen && (!asked || (asked->width > 0 && asked->height > 0)));
    blur->set_enabled(show_blur);
    // Glass sees the windows under it too; plain blur only the desktop.
    blur->set_use_cache(!is_glass);
    if (!is_glass) {
        blur->set_strength(1.0f);
        blur->set_refraction(0, 0);
        blur->set_glass_shapes(nullptr, 0);
    }
    if (is_glass) {
        const int reach = kGlassShadowReach;
        const int w = geom.width, h = geom.height - top();
        blur->set_position(-reach, top() - reach);
        blur->set_size(w + 2 * reach, h + 2 * reach);
        blur->set_corner_radius(0);
        blur->set_alpha(alpha_);
        apply_glass(blur, *given, float(reach), float(reach), w, h, 1.0, c);
    } else if (show_blur && asked) {
        // In surface coordinates, from the content's corner under the title bar.
        const Box content_area{0, 0, geom.width, geom.height - top()};
        Box b{};
        box_intersection(&b, &*asked, &content_area);
        blur->set_position(b.x, top() + b.y);
        blur->set_size(b.width, b.height);
        blur->set_corner_radius(b.width == geom.width ? radius : 0);
        blur->set_alpha(alpha_);
    } else if (show_blur) {
        blur->set_position(0, 0);
        blur->set_size(geom.width, geom.height);
        blur->set_corner_radius(radius);
        blur->set_alpha(alpha_);
    }

    if (titlebar) {
        titlebar->update();
        titlebar->node()->set_opacity(alpha_);
    }
    update_corners();
}

void View::update_corners() {
    if (!content || unmanaged())
        return;
    RoundCtx ctx{content->x, content->y, geom.width, geom.height - top(), fullscreen ? 0 : server.config.corner_radius,
                 top() == 0, alpha_};
    content->for_each_buffer([&](scene::Buffer* b_, int x_, int y_) { (round_window_corners)(b_, x_, y_, &ctx); });
    if (server.overview)
        server.overview->view_changed(this);
    if (server.switcher)
        server.switcher->view_changed(this);
}

// --- foreign toplevel handles (docks, task switchers, screen sharing) ---------

void View::sync_handle() {
    if (!handle_)
        return;
    wl::ForeignToplevels::Info info;
    info.title = title();
    info.app_id = app_id();
    info.maximized = maximized;
    info.minimized = minimized;
    info.fullscreen = fullscreen;
    info.activated = activated;
    if (output && output->global)
        info.outputs.push_back(output->global.get());
    handle_->update(info);
}

void View::create_toplevel_handles() {
    wl::ForeignToplevels::Info info;
    info.title = title();
    info.app_id = app_id();
    handle_ = server.wl->toplevels->create(info);
    handle_->data = this;
    sync_handle();
    if (View* p = parent(); p && p->handle_)
        handle_->set_parent(p->handle_);

    auto& c = handle_connections_;
    c.push_back(handle_->events.request_activate.connect([this](wl::Seat*) {
        if (minimized)
            set_minimized(false);
        server.focus_view(this);
    }));
    c.push_back(handle_->events.request_maximize.connect([this](bool on) { set_maximized(on); }));
    c.push_back(handle_->events.request_minimize.connect([this](bool on) { set_minimized(on); }));
    c.push_back(handle_->events.request_fullscreen.connect([this](bool on, wl::Output*) { set_fullscreen(on); }));
    c.push_back(handle_->events.request_close.connect([this] { close(); }));

    // A private scene holding just this window, for per-window screen capture.
    capture_scene_ = scene::Scene::create();
    create_content(capture_scene_);
}

void View::destroy_toplevel_handles() {
    handle_connections_.clear();
    // Captures of it stop first, while its handle still names it.
    server.capture_view_gone(this);
    if (handle_) {
        server.wl->toplevels->destroy(handle_);
        handle_ = nullptr;
    }
    if (capture_scene_) {
        capture_scene_->destroy();
        capture_scene_ = nullptr;
    }
}

void View::update_title() {
    sync_handle();
    if (titlebar)
        titlebar->update();
    if (mapped)
        server.notify_window(*this, "changed");
}

} // namespace atrium
