#include "switcher.hpp"

#include "cairo_buffer.hpp"
#include "output.hpp"
#include "overview.hpp"
#include "server.hpp"
#include "seat.hpp"
#include "space.hpp"
#include "view.hpp"
#include "window_copy.hpp"

#include <pango/pangocairo.h>

#include <algorithm>
#include <cmath>
#include <string>

namespace atrium {

namespace {

constexpr int kShowDelayMs = 110;  // a quick tap flips without showing anything
constexpr int kItemHeight = 150;
constexpr int kItemMaxWidth = 260;
constexpr int kItemGap = 18;
constexpr int kPad = 22;
constexpr int kTitleGap = 14;
constexpr int kTitleHeight = 22;
constexpr int kRadius = 20;
constexpr int kSelectPad = 9;

constexpr Color kPanel{0.09f, 0.09f, 0.11f, 0.78f};
constexpr Color kSelection{1.0f, 1.0f, 1.0f, 0.13f};
constexpr Color kBacking{0.07f, 0.07f, 0.08f, 1.0f};

int round_i(double v) {
    return int(std::lround(v));
}

} // namespace

Switcher::Switcher(Server& server) : server_(server) {}

Switcher::~Switcher() {
    hide();
    if (delay_)
        wl_event_source_remove(delay_);
}

void Switcher::step(int direction, uint32_t hold) {
    if (server_.locked || server_.overview->active())
        return;
    if (!active_) {
        // Windows of what is on screen here: the shown space, or the secret
        // space over it.
        Output* o = server_.focused_output;
        const Space* only = (server_.shown_secret && server_.shown_secret->output == o) ? server_.shown_secret : nullptr;
        views_.clear();
        for (View* v : server_.views) {
            if (v->unmanaged() || !v->mapped || v->output != o || !v->space || v->hidden_from_lists())
                continue;
            if (only ? v->space != only : !v->space->shown())
                continue;
            views_.push_back(v);
        }
        if (views_.size() < 2)
            return;
        active_ = true;
        hold_ = hold;
        index_ = 0;
        if (!delay_)
            delay_ = wl_event_loop_add_timer(server_.loop, [](void* data) {
                static_cast<Switcher*>(data)->show();
                return 0;
            }, this);
        wl_event_source_timer_update(delay_, kShowDelayMs);
    }
    const int n = int(views_.size());
    select(((index_ + direction) % n + n) % n);
    if (!hold_)
        commit();  // nothing to hold: a plain focus switch
}

void Switcher::modifiers(uint32_t mods) {
    if (active_ && !(mods & hold_))
        commit();
}

void Switcher::key(xkb_keysym_t sym) {
    if (!active_)
        return;
    switch (sym) {
    case XKB_KEY_Escape: cancel(); break;
    case XKB_KEY_Return:
    case XKB_KEY_KP_Enter: commit(); break;
    case XKB_KEY_Left: select((index_ - 1 + int(views_.size())) % int(views_.size())); break;
    case XKB_KEY_Right: select((index_ + 1) % int(views_.size())); break;
    default: break;
    }
}

void Switcher::motion(double lx, double ly) {
    if (!shown_)
        return;
    if (int i = item_at(lx, ly); i >= 0)
        select(i);
}

void Switcher::button(double lx, double ly, bool pressed) {
    if (!active_ || !pressed)
        return;
    if (int i = item_at(lx, ly); i >= 0) {
        select(i);
        commit();
    }
}

void Switcher::commit() {
    View* pick = (index_ >= 0 && index_ < int(views_.size())) ? views_[index_] : nullptr;
    cancel();
    if (!pick)
        return;
    if (pick->minimized)
        pick->set_minimized(false);
    server_.focus_view(pick);
}

void Switcher::cancel() {
    active_ = false;
    if (delay_)
        wl_event_source_timer_update(delay_, 0);
    hide();
    views_.clear();
}

// --- the panel -------------------------------------------------------------------

void Switcher::show() {
    if (!active_ || shown_)
        return;
    Output* o = server_.focused_output;
    if (!o)
        return;
    shown_ = true;
    root_ = wlr_scene_tree_create(server_.layer(Layer::Overview));
    blur_ = wlr_scene_blur_create(root_, 0, 0);
    wlr_scene_blur_set_should_only_blur_bottom_layer(blur_, false);
    wlr_scene_node_set_enabled(&blur_->node, server_.config.blur);
    panel_ = wlr_scene_rect_create(root_, 0, 0, premultiplied(kPanel).data());
    selection_ = wlr_scene_rect_create(root_, 0, 0, premultiplied(kSelection).data());
    for (View* v : views_) {
        auto item = std::make_unique<Item>();
        item->view = v;
        item->tree = wlr_scene_tree_create(root_);
        if (!server_.config.transparency)
            wlr_scene_rect_create(item->tree, 0, 0, premultiplied(kBacking).data());
        item->copy = std::make_unique<WindowCopy>(*v, item->tree);
        items_.push_back(std::move(item));
    }
    title_ = wlr_scene_buffer_create(root_, nullptr);
    layout();
    select(index_);
    server_.seat->refresh_pointer();
}

void Switcher::hide() {
    if (!shown_)
        return;
    shown_ = false;
    for (auto& it : items_)
        it->copy.reset();  // before the trees holding them
    items_.clear();
    wlr_scene_node_destroy(&root_->node);
    root_ = nullptr;
    blur_ = nullptr;
    panel_ = selection_ = nullptr;
    title_ = nullptr;
}

void Switcher::layout() {
    Output* o = server_.focused_output;
    if (!o || items_.empty())
        return;
    // Every preview the same height, as wide as its window's shape asks,
    // shrunk together until the row fits the screen.
    std::vector<double> aspect;
    double total = 0;
    for (auto& it : items_) {
        const double a = std::clamp(it->view->geom.width / double(std::max(1, it->view->geom.height)), 0.5, 2.2);
        aspect.push_back(a);
        total += std::min(a * kItemHeight, double(kItemMaxWidth));
    }
    const double room = o->box.width - 2 * (kPad + 40) - kItemGap * (items_.size() - 1);
    const double s = std::min(1.0, room / total);
    const int h = std::max(24, round_i(kItemHeight * s));

    int row_w = 0;
    std::vector<int> widths;
    for (double a : aspect) {
        widths.push_back(std::max(16, round_i(std::min(a * kItemHeight, double(kItemMaxWidth)) * s)));
        row_w += widths.back();
    }
    row_w += kItemGap * int(items_.size() - 1);

    const int pw = row_w + 2 * kPad;
    const int ph = h + 2 * kPad + kTitleGap + kTitleHeight;
    panel_box_ = {o->box.x + (o->box.width - pw) / 2, o->box.y + (o->box.height - ph) / 2, pw, ph};

    wlr_scene_node_set_position(&blur_->node, panel_box_.x, panel_box_.y);
    wlr_scene_blur_set_size(blur_, pw, ph);
    wlr_scene_blur_set_corner_radius(blur_, kRadius);
    wlr_scene_node_set_position(&panel_->node, panel_box_.x, panel_box_.y);
    wlr_scene_rect_set_size(panel_, pw, ph);
    wlr_scene_rect_set_corner_radius(panel_, kRadius);

    int x = panel_box_.x + kPad;
    const int y = panel_box_.y + kPad;
    const int radius = std::max(4, server_.config.corner_radius / 2);
    for (size_t i = 0; i < items_.size(); ++i) {
        Item& it = *items_[i];
        const View& v = *it.view;
        // Keep the window's own shape inside its slot.
        const double va = v.geom.width / double(std::max(1, v.geom.height));
        int iw = widths[i], ih = h;
        if (va > iw / double(ih))
            ih = std::max(1, round_i(iw / va));
        else
            iw = std::max(1, round_i(ih * va));
        it.box = {x + (widths[i] - iw) / 2, y + (h - ih) / 2, iw, ih};
        wlr_scene_node_set_position(&it.tree->node, it.box.x, it.box.y);
        it.copy->place(iw, ih);
        wlr_scene_node* first = wl_list_empty(&it.tree->children) ? nullptr
            : wl_container_of(it.tree->children.next, first, link);
        if (first && first->type == WLR_SCENE_NODE_RECT) {
            auto* back = wlr_scene_rect_from_node(first);
            wlr_scene_rect_set_size(back, iw, ih);
            wlr_scene_rect_set_corner_radius(back, radius);
        }
        x += widths[i] + kItemGap;
    }
}

void Switcher::select(int index) {
    index_ = index;
    if (!shown_ || index < 0 || index >= int(items_.size()))
        return;
    const wlr_box& b = items_[index]->box;
    wlr_scene_node_set_position(&selection_->node, b.x - kSelectPad, b.y - kSelectPad);
    wlr_scene_rect_set_size(selection_, b.width + 2 * kSelectPad, b.height + 2 * kSelectPad);
    wlr_scene_rect_set_corner_radius(selection_, 12);
    render_title();
}

void Switcher::render_title() {
    if (!title_ || index_ < 0 || index_ >= int(items_.size()))
        return;
    View* v = items_[index_]->view;
    std::string text = v->title() ? v->title() : "";
    if (text.empty() && v->app_id())
        text = v->app_id();
    const float scale = server_.focused_output ? server_.focused_output->wlr->scale : 1.0f;
    const int w = std::max(1, panel_box_.width - 2 * kPad), h = kTitleHeight;

    cairo_surface_t* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, int(std::ceil(w * scale)),
                                                          int(std::ceil(h * scale)));
    cairo_t* cr = cairo_create(surface);
    cairo_scale(cr, scale, scale);
    PangoLayout* layout = pango_cairo_create_layout(cr);
    PangoFontDescription* font = pango_font_description_from_string("Sans Semi-Bold 10.5");
    pango_layout_set_font_description(layout, font);
    pango_font_description_free(font);
    pango_layout_set_text(layout, text.c_str(), -1);
    pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
    pango_layout_set_width(layout, w * PANGO_SCALE);
    pango_layout_set_alignment(layout, PANGO_ALIGN_CENTER);
    pango_layout_set_single_paragraph_mode(layout, true);
    int tw = 0, th = 0;
    pango_layout_get_pixel_size(layout, &tw, &th);
    cairo_set_source_rgba(cr, 1, 1, 1, 0.9);
    cairo_move_to(cr, 0, (h - th) / 2.0);
    pango_cairo_show_layout(cr, layout);
    g_object_unref(layout);
    cairo_destroy(cr);
    cairo_surface_flush(surface);
    wlr_buffer_unlock(set_cairo_buffer(title_, surface, w, h));
    wlr_scene_node_set_position(&title_->node, panel_box_.x + kPad,
                                panel_box_.y + panel_box_.height - kPad - kTitleHeight + 4);
}

int Switcher::item_at(double lx, double ly) const {
    for (size_t i = 0; i < items_.size(); ++i) {
        const wlr_box& b = items_[i]->box;
        if (lx >= b.x - kSelectPad && lx < b.x + b.width + kSelectPad &&
            ly >= b.y - kSelectPad && ly < b.y + b.height + kSelectPad)
            return int(i);
    }
    return -1;
}

// --- window changes --------------------------------------------------------------

void Switcher::view_changed(View* view) {
    for (auto& it : items_)
        if (it->view == view && it->copy)
            it->copy->refresh();
}

void Switcher::view_unmapped(View* view) {
    if (!active_ || std::ranges::find(views_, view) == views_.end())
        return;
    // Simplest honest answer: start over without it.
    const bool was_shown = shown_;
    View* current = views_[index_];
    hide();
    std::erase(views_, view);
    if (views_.size() < 2) {
        cancel();
        return;
    }
    auto it = std::ranges::find(views_, current);
    index_ = it == views_.end() ? 0 : int(it - views_.begin());
    if (was_shown)
        show();
}

} // namespace atrium
