#include "overview.hpp"

#include "cairo_buffer.hpp"
#include "geometry.hpp"
#include "layer_surface.hpp"
#include "output.hpp"
#include "seat.hpp"
#include "server.hpp"
#include "space.hpp"
#include "view.hpp"

#include <pango/pangocairo.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>

namespace atrium {

namespace {

// Room around the grid, and between thumbnails.
constexpr int kPadSide = 56;
constexpr int kStripTop = 18;      // space tiles, from the top of the usable area
constexpr int kTileHeight = 68;
constexpr int kTileGap = 16;
constexpr int kTileRadius = 8;
constexpr int kPadTop = kStripTop + kTileHeight + 40;
constexpr int kPadBottom = 56;
constexpr int kGap = 32;
constexpr int kLabelGap = 10;     // thumbnail to title pill
constexpr int kLabelHeight = 24;
constexpr int kLabelPadX = 12;
constexpr int kLabelMaxText = 320;
constexpr int kRing = 3;           // highlight ring width
constexpr float kShadowSigma = 18.0f;

constexpr Color kDim{0.0f, 0.0f, 0.0f, 0.22f};
constexpr Color kRingColor{0.04f, 0.52f, 1.0f, 0.95f};  // system blue
constexpr Color kShadow{0.0f, 0.0f, 0.0f, 0.45f};
constexpr Color kBacking{0.07f, 0.07f, 0.08f, 1.0f};  // as behind real windows
constexpr Color kTileCurrent{1.0f, 1.0f, 1.0f, 0.85f};
constexpr Color kNewTile{1.0f, 1.0f, 1.0f, 0.10f};
constexpr Color kPlus{1.0f, 1.0f, 1.0f, 0.75f};
constexpr double kDragThreshold = 6;

int round_i(double v) {
    return int(std::lround(v));
}

wlr_box lerp(const wlr_box& a, const wlr_box& b, double t) {
    return {round_i(a.x + (b.x - a.x) * t), round_i(a.y + (b.y - a.y) * t),
            std::max(1, round_i(a.width + (b.width - a.width) * t)),
            std::max(1, round_i(a.height + (b.height - a.height) * t))};
}

uint16_t scaled(uint16_t r, double s) {
    return uint16_t(std::max(0L, std::lround(r * s)));
}

} // namespace

Overview::Overview(Server& server) : server_(server) {}

Overview::~Overview() {
    server_.animator.cancel_owner(this, false);
    server_.animator.cancel_owner(&screens_, false);
    destroy_all();
}

bool Overview::included(View* v) const {
    return v->mapped && !v->minimized && !v->unmanaged() && v->space && !v->space->secret &&
           v->space->shown() && v->output && v->tree;
}

Overview::Screen* Overview::screen_for(Output* output) {
    for (auto& s : screens_)
        if (s->output == output)
            return s.get();
    return nullptr;
}

Overview::Thumb* Overview::thumb_for(View* view) {
    for (auto& t : thumbs_)
        if (t->view == view)
            return t.get();
    return nullptr;
}

Overview::Thumb* Overview::thumb_at(double lx, double ly) {
    // Topmost first: later thumbs sit above earlier ones.
    for (auto it = thumbs_.rbegin(); it != thumbs_.rend(); ++it) {
        const wlr_box& b = (*it)->cur;
        if (lx >= b.x && lx < b.x + b.width && ly >= b.y && ly < b.y + b.height)
            return it->get();
    }
    return nullptr;
}

// --- open / close ------------------------------------------------------------------

void Overview::toggle() {
    if (state_ == State::Open)
        close();
    else
        open();
}

void Overview::open(bool animate) {
    if (state_ == State::Open || server_.locked)
        return;
    if (state_ == State::Closing) {
        server_.animator.cancel_owner(&screens_, false);
        server_.animator.cancel_owner(this, true);  // finishes the close
    }
    server_.seat->cancel_grab();
    server_.hide_secret();

    root_ = wlr_scene_tree_create(server_.layer(Layer::Overview));
    for (Output* o : server_.outputs) {
        if (!o->enabled())
            continue;
        auto s = std::make_unique<Screen>();
        s->output = o;
        s->tree = wlr_scene_tree_create(root_);
        // The desktop frosts over; windows turn into thumbnails above it.
        s->blur = wlr_scene_blur_create(s->tree, o->box.width, o->box.height);
        wlr_scene_blur_set_should_only_blur_bottom_layer(s->blur, false);
        wlr_scene_node_set_position(&s->blur->node, o->box.x, o->box.y);
        s->dim = wlr_scene_rect_create(s->tree, o->box.width, o->box.height, premultiplied(kDim).data());
        wlr_scene_node_set_position(&s->dim->node, o->box.x, o->box.y);
        screens_.push_back(std::move(s));
    }
    state_ = State::Open;
    fade_ = 0;
    set_fade(0);
    rebuild_strips();

    // Least recently focused first, so the focused window ends up on top
    // while they fly into place.
    for (auto it = server_.views.rbegin(); it != server_.views.rend(); ++it)
        if (included(*it))
            if (Screen* s = screen_for((*it)->output))
                add_thumb(*it, s);

    relayout(animate);
    if (animate)
        server_.animator.start(&screens_, 260, Ease::OutCubic, [this](double t) { set_fade(t); });
    else
        set_fade(1);
    if (server_.focused_view)
        set_highlight(thumb_for(server_.focused_view));
    server_.seat->set_default_cursor();
    wlr_seat_pointer_notify_clear_focus(server_.seat->wlr);
}

void Overview::close(View* pick) {
    if (state_ != State::Open)
        return;
    state_ = State::Closing;
    set_highlight(nullptr);
    if (pick) {
        if (Thumb* t = thumb_for(pick))
            wlr_scene_node_raise_to_top(&t->tree->node);
        server_.focus_view(pick);
        if (state_ != State::Closing)
            return;  // focusing tore the overview down
    }

    for (auto& t : thumbs_) {
        t->from = t->cur;
        t->to = t->view->geom;
    }
    server_.animator.cancel_owner(this, false);
    server_.animator.cancel_owner(&screens_, false);
    const double fade_from = fade_;
    server_.animator.start(&screens_, 240, Ease::OutCubic, [this, fade_from](double t) {
        set_fade(fade_from * (1 - t));
    });
    server_.animator.start(this, 260, Ease::OutCubic, [this](double t) {
        for (auto& th : thumbs_)
            place(*th, lerp(th->from, th->to, t));
    }, [this] { finish_close(); });
}

void Overview::finish_close() {
    server_.animator.cancel_owner(&screens_, false);
    std::vector<View*> views;
    for (auto& t : thumbs_)
        views.push_back(t->view);
    destroy_all();
    state_ = State::Closed;
    for (View* v : views)
        v->set_alpha(1.0f);
    server_.seat->refresh_pointer();
}

void Overview::close_now() {
    if (state_ == State::Closed)
        return;
    server_.animator.cancel_owner(this, false);
    finish_close();
}

void Overview::destroy_all() {
    if (root_)
        wlr_scene_node_destroy(&root_->node);
    root_ = nullptr;
    highlight_ = nullptr;
    tile_hover_ = press_tile_ = nullptr;
    press_thumb_ = drag_ = nullptr;
    pressed_ = false;
    tiles_.clear();
    thumbs_.clear();
    screens_.clear();
}

void Overview::set_fade(double a) {
    fade_ = a;
    for (auto& s : screens_) {
        wlr_scene_blur_set_alpha(s->blur, float(a));
        Color c = kDim;
        c[3] *= float(a);
        wlr_scene_rect_set_color(s->dim, premultiplied(c).data());
    }
}

// --- thumbnails ----------------------------------------------------------------------

void Overview::add_thumb(View* view, Screen* screen) {
    auto t = std::make_unique<Thumb>();
    t->view = view;
    t->screen = screen;
    t->tree = wlr_scene_tree_create(screen->tree);
    t->shadow = wlr_scene_shadow_create(t->tree, 0, 0, 0, kShadowSigma, kShadow.data());
    t->ring = wlr_scene_rect_create(t->tree, 0, 0, premultiplied(kRingColor).data());
    wlr_scene_node_set_enabled(&t->ring->node, false);
    const Config& c = server_.config;
    t->blur = wlr_scene_blur_create(t->tree, 0, 0);
    wlr_scene_node_set_enabled(&t->blur->node, c.blur && c.transparency);
    t->backing = wlr_scene_rect_create(t->tree, 0, 0, premultiplied(kBacking).data());
    wlr_scene_node_set_enabled(&t->backing->node, !c.transparency);
    t->pieces_tree = wlr_scene_tree_create(t->tree);
    t->label = wlr_scene_buffer_create(t->tree, nullptr);
    wlr_scene_node_set_enabled(&t->label->node, false);
    t->from = t->to = t->cur = view->geom;

    // The real window stays put, drawing (so the copy stays live) but unseen.
    server_.animator.cancel_owner(view, true);
    view->set_alpha(0.0f);

    Thumb& ref = *t;
    thumbs_.push_back(std::move(t));
    snapshot(ref);
    place(ref, ref.cur);
}

void Overview::remove_thumb(Thumb* thumb) {
    if (highlight_ == thumb)
        highlight_ = nullptr;
    if (press_thumb_ == thumb)
        press_thumb_ = nullptr;
    if (drag_ == thumb)
        drag_ = nullptr;
    wlr_scene_node_destroy(&thumb->tree->node);
    std::erase_if(thumbs_, [thumb](const auto& t) { return t.get() == thumb; });
}

namespace {

struct CopyCtx {
    wlr_scene_tree* into;
    int ox, oy;
    std::vector<wlr_scene_buffer*>* nodes;
    std::vector<std::array<int, 4>>* boxes;
    std::vector<fx_corner_radii>* corners;
};

void copy_buffer(wlr_scene_buffer* src, int sx, int sy, void* data) {
    auto* c = static_cast<CopyCtx*>(data);
    if (!src->buffer)
        return;
    wlr_scene_buffer* dst = wlr_scene_buffer_create(c->into, src->buffer);
    wlr_scene_buffer_set_source_box(dst, &src->src_box);
    wlr_scene_buffer_set_transform(dst, src->transform);
    int w = src->dst_width, h = src->dst_height;
    if (w <= 0 || h <= 0) {
        w = src->buffer->width;
        h = src->buffer->height;
    }
    c->nodes->push_back(dst);
    c->boxes->push_back({sx - c->ox, sy - c->oy, w, h});
    c->corners->push_back(src->corners);
}

} // namespace

// Copy what the window shows right now. The copies hold references to the
// client's buffers, so they stay valid after the client moves on.
void Overview::snapshot(Thumb& t) {
    wlr_scene_node_destroy(&t.pieces_tree->node);
    t.pieces_tree = wlr_scene_tree_create(t.tree);
    wlr_scene_node_place_below(&t.pieces_tree->node, &t.label->node);
    t.pieces.clear();

    std::vector<wlr_scene_buffer*> nodes;
    std::vector<std::array<int, 4>> boxes;
    std::vector<fx_corner_radii> corners;
    CopyCtx ctx{t.pieces_tree, t.view->tree->node.x, t.view->tree->node.y, &nodes, &boxes, &corners};
    wlr_scene_node_for_each_buffer(&t.view->tree->node, copy_buffer, &ctx);
    for (size_t i = 0; i < nodes.size(); ++i)
    for (size_t i = 0; i < nodes.size(); ++i)
        t.pieces.push_back({nodes[i], boxes[i][0], boxes[i][1], boxes[i][2], boxes[i][3], corners[i]});
}

void Overview::place(Thumb& t, const wlr_box& box) {
    t.cur = box;
    const View& v = *t.view;
    const double sx = box.width / double(std::max(1, v.geom.width));
    const double sy = box.height / double(std::max(1, v.geom.height));
    wlr_scene_node_set_position(&t.tree->node, box.x, box.y);

    for (Piece& p : t.pieces) {
        wlr_scene_node_set_position(&p.node->node, round_i(p.x * sx), round_i(p.y * sy));
        wlr_scene_buffer_set_dest_size(p.node, std::max(1, round_i(p.w * sx)), std::max(1, round_i(p.h * sy)));
        wlr_scene_buffer_set_corner_radii(p.node, corner_radii_new(
            scaled(p.corners.top_left, sx), scaled(p.corners.top_right, sx),
            scaled(p.corners.bottom_right, sx), scaled(p.corners.bottom_left, sx)));
    }

    const int radius = v.fullscreen ? 0 : std::max(1, round_i(server_.config.corner_radius * sx));
    const int m = int(std::ceil(kShadowSigma));
    wlr_scene_shadow_set_size(t.shadow, box.width + 2 * m, box.height + 2 * m);
    wlr_scene_shadow_set_corner_radius(t.shadow, radius);
    wlr_scene_node_set_position(&t.shadow->node, -m, -m);
    wlr_scene_shadow_set_clipped_region(t.shadow, clipped_region{
        .area = {m, m, box.width, box.height},
        .corners = corner_radii_all(radius),
    });

    wlr_scene_rect_set_size(t.ring, box.width + 2 * kRing, box.height + 2 * kRing);
    wlr_scene_node_set_position(&t.ring->node, -kRing, -kRing);
    wlr_scene_rect_set_corner_radius(t.ring, radius + kRing);
    wlr_scene_rect_set_clipped_region(t.ring, clipped_region{
        .area = {kRing, kRing, box.width, box.height},
        .corners = corner_radii_all(radius),
    });

    wlr_scene_blur_set_size(t.blur, box.width, box.height);
    wlr_scene_blur_set_corner_radius(t.blur, radius);
    wlr_scene_rect_set_size(t.backing, box.width, box.height);
    wlr_scene_rect_set_corner_radius(t.backing, radius);

    wlr_scene_node_set_position(&t.label->node, (box.width - t.label_w) / 2, box.height + kLabelGap);
}

void Overview::relayout(bool animate) {
    for (auto& s : screens_) {
        std::vector<Thumb*> mine;
        std::vector<wlr_box> sizes;
        for (auto& t : thumbs_)
            if (t->screen == s.get()) {
                mine.push_back(t.get());
                sizes.push_back(t->view->geom);
            }
        const wlr_box& u = s->output->usable;
        const wlr_box area{u.x + kPadSide, u.y + kPadTop, u.width - 2 * kPadSide, u.height - kPadTop - kPadBottom};
        const auto boxes = geometry::overview_layout(sizes, area, kGap, kLabelGap + kLabelHeight);
        for (size_t i = 0; i < mine.size(); ++i) {
            mine[i]->from = animate ? mine[i]->cur : boxes[i];
            mine[i]->to = boxes[i];
            mine[i]->laid_w = mine[i]->view->geom.width;
            mine[i]->laid_h = mine[i]->view->geom.height;
        }
    }
    server_.animator.cancel_owner(this, false);
    server_.animator.start(this, animate ? 320 : 0, Ease::OutQuint, [this](double t) {
        for (auto& th : thumbs_)
            if (th.get() != drag_)  // the dragged one follows the pointer
                place(*th, lerp(th->from, th->to, t));
    });
}

void Overview::set_highlight(Thumb* t) {
    if (t == highlight_)
        return;
    if (highlight_) {
        wlr_scene_node_set_enabled(&highlight_->ring->node, false);
        wlr_scene_node_set_enabled(&highlight_->label->node, false);
    }
    highlight_ = t;
    if (!t)
        return;
    wlr_scene_node_set_enabled(&t->ring->node, true);
    render_label(*t);
    wlr_scene_node_set_enabled(&t->label->node, true);
    place(*t, t->cur);
}

// The window's title in a dark pill, drawn at the output's scale.
void Overview::render_label(Thumb& t) {
    std::string text = t.view->title() ? t.view->title() : "";
    if (text.empty() && t.view->app_id())
        text = t.view->app_id();
    if (text.empty())
        text = "Window";
    const float scale = t.screen->output->wlr->scale;

    PangoFontDescription* font = pango_font_description_from_string("Sans Semi-Bold 10");
    cairo_surface_t* probe = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
    cairo_t* pcr = cairo_create(probe);
    PangoLayout* layout = pango_cairo_create_layout(pcr);
    pango_layout_set_font_description(layout, font);
    pango_layout_set_text(layout, text.c_str(), -1);
    pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
    pango_layout_set_width(layout, kLabelMaxText * PANGO_SCALE);
    pango_layout_set_single_paragraph_mode(layout, true);
    int tw = 0, th = 0;
    pango_layout_get_pixel_size(layout, &tw, &th);
    g_object_unref(layout);
    cairo_destroy(pcr);
    cairo_surface_destroy(probe);

    const int w = tw + 2 * kLabelPadX, h = kLabelHeight;
    cairo_surface_t* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, int(std::ceil(w * scale)),
                                                          int(std::ceil(h * scale)));
    cairo_t* cr = cairo_create(surface);
    cairo_scale(cr, scale, scale);

    const double r = h / 2.0;
    cairo_new_sub_path(cr);
    cairo_arc(cr, w - r, r, r, -M_PI / 2, M_PI / 2);
    cairo_arc(cr, r, r, r, M_PI / 2, 3 * M_PI / 2);
    cairo_close_path(cr);
    cairo_set_source_rgba(cr, 0.11, 0.11, 0.13, 0.88);
    cairo_fill_preserve(cr);
    cairo_set_source_rgba(cr, 1, 1, 1, 0.10);
    cairo_set_line_width(cr, 1);
    cairo_stroke(cr);

    layout = pango_cairo_create_layout(cr);
    pango_layout_set_font_description(layout, font);
    pango_layout_set_text(layout, text.c_str(), -1);
    pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
    pango_layout_set_width(layout, kLabelMaxText * PANGO_SCALE);
    pango_layout_set_single_paragraph_mode(layout, true);
    cairo_set_source_rgba(cr, 1, 1, 1, 0.92);
    cairo_move_to(cr, kLabelPadX, (h - th) / 2.0);
    pango_cairo_show_layout(cr, layout);
    g_object_unref(layout);
    pango_font_description_free(font);

    cairo_destroy(cr);
    cairo_surface_flush(surface);
    wlr_buffer_unlock(set_cairo_buffer(t.label, surface, w, h));
    t.label_w = w;
}

// --- input ---------------------------------------------------------------------------

void Overview::motion(double lx, double ly) {
    if (state_ != State::Open)
        return;
    if (press_thumb_ && !drag_ && std::hypot(lx - press_x_, ly - press_y_) >= kDragThreshold) {
        drag_ = press_thumb_;
        drag_rx_ = (press_x_ - drag_->cur.x) / std::max(1, drag_->cur.width);
        drag_ry_ = (press_y_ - drag_->cur.y) / std::max(1, drag_->cur.height);
        wlr_scene_node_raise_to_top(&drag_->tree->node);
        wlr_scene_node_raise_to_top(&drag_->screen->tree->node);
        set_highlight(nullptr);
    }
    if (drag_) {
        // Shrinks on the way up to the strip so the tiles stay in sight.
        const wlr_box& full = drag_->to;
        const Output* o = drag_->screen->output;
        const double strip_bottom = o->usable.y + kStripTop + kTileHeight;
        const double f = std::clamp((ly - strip_bottom) / 280.0, 0.0, 1.0);
        const double tile_w = kTileHeight * double(o->box.width) / std::max(1, o->box.height);
        const double small_w = std::min<double>(full.width, tile_w * 1.25);
        const double w = small_w + (full.width - small_w) * f;
        const double h = w * full.height / std::max(1, full.width);
        place(*drag_, {round_i(lx - drag_rx_ * w), round_i(ly - drag_ry_ * h),
                       std::max(1, round_i(w)), std::max(1, round_i(h))});
        // Over a tile the strip comes to the front: the window slips into it.
        Tile* tile = tile_at(lx, ly);
        set_tile_hover(tile);
        for (auto& s : screens_) {
            if (tile && tile->screen == s.get())
                wlr_scene_node_raise_to_top(&s->strip->node);
            else
                wlr_scene_node_place_above(&s->strip->node, &s->dim->node);
        }
        return;
    }
    Tile* tile = tile_at(lx, ly);
    set_tile_hover(tile);
    set_highlight(tile ? nullptr : thumb_at(lx, ly));
}

void Overview::button(double lx, double ly, uint32_t button, bool pressed) {
    if (state_ != State::Open || button != BTN_LEFT)
        return;
    if (pressed) {
        pressed_ = true;
        press_x_ = lx;
        press_y_ = ly;
        press_tile_ = tile_at(lx, ly);
        press_thumb_ = press_tile_ ? nullptr : thumb_at(lx, ly);
        return;
    }
    if (!pressed_)
        return;  // pressed before the overview opened
    pressed_ = false;
    Thumb* thumb = press_thumb_;
    Tile* tile = press_tile_;
    press_thumb_ = nullptr;
    press_tile_ = nullptr;

    if (drag_) {
        Thumb* dragged = drag_;
        drag_ = nullptr;
        drop(dragged, lx, ly);
        return;
    }
    if (thumb) {
        if (thumb_at(lx, ly) == thumb)
            close(thumb->view);
        return;
    }
    if (tile) {
        if (tile_at(lx, ly) == tile)
            go_to_space(tile->screen->output, tile->number);
        return;
    }
    close();
}

void Overview::key(xkb_keysym_t sym) {
    if (state_ != State::Open)
        return;
    switch (sym) {
    case XKB_KEY_Escape:
        close();
        break;
    case XKB_KEY_Return:
    case XKB_KEY_KP_Enter:
    case XKB_KEY_space:
        close(highlight_ ? highlight_->view : nullptr);
        break;
    case XKB_KEY_Left: navigate(-1, 0); break;
    case XKB_KEY_Right: navigate(1, 0); break;
    case XKB_KEY_Up: navigate(0, -1); break;
    case XKB_KEY_Down: navigate(0, 1); break;
    default: break;
    }
}

// Move the highlight to the nearest thumbnail in a direction.
void Overview::navigate(int dx, int dy) {
    if (thumbs_.empty())
        return;
    if (!highlight_) {
        set_highlight(thumbs_.back().get());
        return;
    }
    auto center = [](const wlr_box& b) { return std::pair{b.x + b.width / 2.0, b.y + b.height / 2.0}; };
    const auto [cx, cy] = center(highlight_->to);
    Thumb* best = nullptr;
    double best_score = std::numeric_limits<double>::max();
    for (auto& t : thumbs_) {
        if (t.get() == highlight_)
            continue;
        const auto [x, y] = center(t->to);
        const double along = (x - cx) * dx + (y - cy) * dy;
        if (along <= 0)
            continue;
        const double across = std::abs(dx ? y - cy : x - cx);
        const double score = along + 2 * across;
        if (score < best_score) {
            best_score = score;
            best = t.get();
        }
    }
    if (best)
        set_highlight(best);
}

// --- spaces strip ---------------------------------------------------------------------

namespace {

// Copy every buffer under `root` into `into`, shrunk by `s` around `origin`
// (layout coordinates), for a miniature.
struct MiniCtx {
    wlr_scene_tree* into;
    int root_x, root_y;     // the root's own position, which for_each_buffer counts
    double ox, oy;          // where the root sits in layout coordinates
    double origin_x, origin_y;
    double s;
    int radius;             // corner radius for buffers that fill the output
    int full_w, full_h;
};

void copy_mini(wlr_scene_buffer* src, int sx, int sy, void* data) {
    auto* c = static_cast<MiniCtx*>(data);
    if (!src->buffer)
        return;
    int w = src->dst_width, h = src->dst_height;
    if (w <= 0 || h <= 0) {
        w = src->buffer->width;
        h = src->buffer->height;
    }
    const double lx = c->ox + (sx - c->root_x), ly = c->oy + (sy - c->root_y);
    wlr_scene_buffer* dst = wlr_scene_buffer_create(c->into, src->buffer);
    wlr_scene_buffer_set_source_box(dst, &src->src_box);
    wlr_scene_buffer_set_transform(dst, src->transform);
    wlr_scene_node_set_position(&dst->node, round_i((lx - c->origin_x) * c->s), round_i((ly - c->origin_y) * c->s));
    wlr_scene_buffer_set_dest_size(dst, std::max(1, round_i(w * c->s)), std::max(1, round_i(h * c->s)));
    if (w >= c->full_w && h >= c->full_h)
        wlr_scene_buffer_set_corner_radii(dst, corner_radii_all(c->radius));
    else
        wlr_scene_buffer_set_corner_radii(dst, corner_radii_all(scaled(src->corners.top_left, c->s)));
}

} // namespace

void Overview::rebuild_strips() {
    tile_hover_ = press_tile_ = nullptr;
    std::erase_if(tiles_, [](const auto&) { return true; });
    for (auto& s : screens_)
        build_strip(*s);
}

void Overview::spaces_changed() {
    if (state_ == State::Open)
        rebuild_strips();
}

void Overview::build_strip(Screen& sc) {
    if (sc.strip)
        wlr_scene_node_destroy(&sc.strip->node);
    sc.strip = wlr_scene_tree_create(sc.tree);
    wlr_scene_node_place_above(&sc.strip->node, &sc.dim->node);

    Output* o = sc.output;
    std::vector<Space*> spaces;
    for (const auto& sp : server_.spaces)
        if (!sp->secret && sp->output == o)
            spaces.push_back(sp.get());
    std::ranges::sort(spaces, [](Space* a, Space* b) { return a->number < b->number; });
    const int next = spaces.empty() ? 1 : spaces.back()->number + 1;

    const double s = double(kTileHeight) / std::max(1, o->box.height);
    const int tw = std::max(1, round_i(o->box.width * s));
    const int count = int(spaces.size()) + 1;
    const int strip_w = count * tw + (count - 1) * kTileGap;
    int x = o->box.x + (o->box.width - strip_w) / 2;
    const int y = o->usable.y + kStripTop;
    const Config& c = server_.config;

    auto make_tile = [&](int number, bool current) {
        auto t = std::make_unique<Tile>();
        t->screen = &sc;
        t->number = number;
        t->current = current;
        t->box = {x, y, tw, kTileHeight};
        t->tree = wlr_scene_tree_create(sc.strip);
        wlr_scene_node_set_position(&t->tree->node, x, y);
        auto* shadow = wlr_scene_shadow_create(t->tree, tw + 16, kTileHeight + 16, kTileRadius, 8.0f, kShadow.data());
        wlr_scene_node_set_position(&shadow->node, -8, -8);
        wlr_scene_shadow_set_clipped_region(shadow, clipped_region{
            .area = {8, 8, tw, kTileHeight}, .corners = corner_radii_all(kTileRadius)});
        t->ring = wlr_scene_rect_create(t->tree, tw + 2 * kRing, kTileHeight + 2 * kRing,
                                        premultiplied(current ? kTileCurrent : kRingColor).data());
        wlr_scene_node_set_position(&t->ring->node, -kRing, -kRing);
        wlr_scene_rect_set_corner_radius(t->ring, kTileRadius + kRing);
        wlr_scene_rect_set_clipped_region(t->ring, clipped_region{
            .area = {kRing, kRing, tw, kTileHeight}, .corners = corner_radii_all(kTileRadius)});
        wlr_scene_node_set_enabled(&t->ring->node, current);
        x += tw + kTileGap;
        return t;
    };

    for (Space* sp : spaces) {
        auto t = make_tile(sp->number, sp == o->active);
        // The desktop: background color, then wallpaper and bottom panels.
        auto* bg = wlr_scene_rect_create(t->tree, tw, kTileHeight, c.background.data());
        wlr_scene_rect_set_corner_radius(bg, kTileRadius);
        auto* mini = wlr_scene_tree_create(t->tree);
        for (int layer = 0; layer < 2; ++layer)
            for (LayerSurface* l : o->layers[layer]) {
                if (!l->tree)
                    continue;
                MiniCtx ctx{mini, l->tree->node.x, l->tree->node.y, double(l->tree->node.x), double(l->tree->node.y),
                            double(o->box.x), double(o->box.y), s, kTileRadius, o->box.width, o->box.height};
                wlr_scene_node_for_each_buffer(&l->tree->node, copy_mini, &ctx);
            }
        // Its windows, bottom of the stack first.
        for (auto it = server_.views.rbegin(); it != server_.views.rend(); ++it) {
            View* v = *it;
            if (v->space != sp || v->minimized || !v->mapped || !v->tree)
                continue;
            if (!c.transparency) {
                auto* back = wlr_scene_rect_create(mini, std::max(1, round_i(v->geom.width * s)),
                                                   std::max(1, round_i(v->geom.height * s)), premultiplied(kBacking).data());
                wlr_scene_node_set_position(&back->node, round_i((v->geom.x - o->box.x) * s), round_i((v->geom.y - o->box.y) * s));
                wlr_scene_rect_set_corner_radius(back, 2);
            }
            MiniCtx ctx{mini, v->tree->node.x, v->tree->node.y, double(v->geom.x), double(v->geom.y),
                        double(o->box.x), double(o->box.y), s, kTileRadius, o->box.width, o->box.height};
            wlr_scene_node_for_each_buffer(&v->tree->node, copy_mini, &ctx);
        }
        tiles_.push_back(std::move(t));
    }

    // "+": a new space.
    auto t = make_tile(0, false);
    auto* bg = wlr_scene_rect_create(t->tree, tw, kTileHeight, premultiplied(kNewTile).data());
    wlr_scene_rect_set_corner_radius(bg, kTileRadius);
    constexpr int kArm = 18, kThick = 2;
    auto* h = wlr_scene_rect_create(t->tree, kArm, kThick, premultiplied(kPlus).data());
    wlr_scene_node_set_position(&h->node, (tw - kArm) / 2, (kTileHeight - kThick) / 2);
    auto* vbar = wlr_scene_rect_create(t->tree, kThick, kArm, premultiplied(kPlus).data());
    wlr_scene_node_set_position(&vbar->node, (tw - kThick) / 2, (kTileHeight - kArm) / 2);
    t->number = -next;  // negative: "+" standing for this number
    tiles_.push_back(std::move(t));
}

Overview::Tile* Overview::tile_at(double lx, double ly) {
    for (auto& t : tiles_) {
        const wlr_box& b = t->box;
        if (lx >= b.x && lx < b.x + b.width && ly >= b.y && ly < b.y + b.height)
            return t.get();
    }
    return nullptr;
}

void Overview::set_tile_hover(Tile* tile) {
    if (tile == tile_hover_)
        return;
    auto show = [](Tile* t, bool hover) {
        if (!t)
            return;
        wlr_scene_node_set_enabled(&t->ring->node, hover || t->current);
        wlr_scene_rect_set_color(t->ring, premultiplied(hover ? kRingColor : kTileCurrent).data());
    };
    show(tile_hover_, false);
    tile_hover_ = tile;
    show(tile, true);
}

// Look at another space without leaving the overview.
void Overview::go_to_space(Output* output, int number) {
    const int n = number < 0 ? -number : number;
    close_now();
    server_.switch_space(output, n);
    server_.animator.cancel_owner(output, true);  // no slide under the overview
    open(false);
}

// A thumbnail let go: onto a space tile it moves there, anywhere else it
// flies back.
void Overview::drop(Thumb* thumb, double lx, double ly) {
    set_tile_hover(nullptr);
    for (auto& s : screens_)
        wlr_scene_node_place_above(&s->strip->node, &s->dim->node);
    Tile* tile = tile_at(lx, ly);
    View* view = thumb->view;
    const int n = tile ? (tile->number < 0 ? -tile->number : tile->number) : 0;
    Output* o = tile ? tile->screen->output : nullptr;
    if (!tile || (view->space && view->space->output == o && view->space->number == n)) {
        relayout();
        return;
    }
    remove_thumb(thumb);
    view->set_alpha(1.0f);  // it lives on a space nobody is looking at now
    server_.move_to_space(view, server_.ensure_space(o, n));
    if (state_ == State::Open) {
        relayout();
        rebuild_strips();
    }
}

// --- window changes ------------------------------------------------------------------

void Overview::view_changed(View* view) {
    if (!active())
        return;
    Thumb* t = thumb_for(view);
    if (!t)
        return;
    snapshot(*t);
    if (state_ == State::Open && (view->geom.width != t->laid_w || view->geom.height != t->laid_h))
        relayout();
    else
        place(*t, t->cur);
}

void Overview::view_mapped(View* view) {
    if (state_ != State::Open || !included(view))
        return;
    if (Screen* s = screen_for(view->output)) {
        add_thumb(view, s);
        relayout();
    }
}

void Overview::view_unmapped(View* view) {
    if (!active())
        return;
    if (Thumb* t = thumb_for(view)) {
        remove_thumb(t);
        if (state_ == State::Open)
            relayout();
    }
}

void Overview::output_removed(Output* output) {
    if (screen_for(output))
        close_now();
}

} // namespace atrium
