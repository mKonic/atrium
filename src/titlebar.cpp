#include "titlebar.hpp"

#include "cairo_buffer.hpp"
#include "output.hpp"
#include "server.hpp"
#include "view.hpp"

#include <cairo.h>
#include <pango/pangocairo.h>

#include <cmath>
#include <functional>

namespace atrium {

namespace {

// --- look ------------------------------------------------------------------------------------

struct Rgba {
    double r, g, b, a = 1.0;
};

constexpr Rgba hex(uint32_t rgb, double a = 1.0) {
    return {((rgb >> 16) & 0xff) / 255.0, ((rgb >> 8) & 0xff) / 255.0, (rgb & 0xff) / 255.0, a};
}

void set(cairo_t* cr, const Rgba& c) {
    cairo_set_source_rgba(cr, c.r, c.g, c.b, c.a);
}

Rgba darker(Rgba c, double f) {
    return {c.r * f, c.g * f, c.b * f, c.a};
}

// Geometry of the bar in logical pixels.
constexpr int kHeight = Titlebar::kHeight;
constexpr double kButton = 12.0;   // diameter
constexpr double kGap = 8.0;       // between buttons
constexpr double kRight = 13.0;    // last button's right edge, from the bar's right
constexpr double kHitPad = 4.0;    // clickable slack around each button

// macOS window chrome, dark and light.
struct Chrome {
    Rgba bar_active, bar_inactive, separator, highlight, title_active, title_inactive, button_inactive;
};
constexpr Chrome kDark{hex(0x26272c), hex(0x1d1e22), hex(0x000000, 0.45), hex(0xffffff, 0.06),
                       hex(0xe8e8ec), hex(0x77787f), hex(0x45464c)};
constexpr Chrome kLight{hex(0xececee), hex(0xf6f6f7), hex(0x000000, 0.14), hex(0xffffff, 0.7),
                        hex(0x2a2a2e), hex(0xa4a4aa), hex(0xd0d0d4)};
constexpr Rgba kClose = hex(0xff5f57);
constexpr Rgba kMinimize = hex(0xfebc2e);
constexpr Rgba kMaximize = hex(0x28c840);
constexpr Rgba kGlyph = hex(0x000000, 0.55);

// Buttons sit at the top right: minimize, maximize, close, with close outermost.
// `index` is 0 = close, 1 = minimize, 2 = maximize; returns the left edge.
double button_x(int index, double width) {
    static constexpr int slot[] = {2, 0, 1};  // position from the left of the group
    const double group = 3 * kButton + 2 * kGap;
    return width - kRight - group + slot[index] * (kButton + kGap);
}

constexpr double kGroupWidth = 3 * kButton + 2 * kGap + kRight;

} // namespace

// --- Titlebar --------------------------------------------------------------------------------

Titlebar::Titlebar(View& view, wlr_scene_tree* parent) : view_(view) {
    buffer_ = wlr_scene_buffer_create(parent, nullptr);
    buffer_->node.data = this;
}

Titlebar::~Titlebar() {
    wlr_scene_node_destroy(&buffer_->node);
    if (held_)
        wlr_buffer_unlock(held_);
}

Titlebar::Part Titlebar::part_at(double x, double y) const {
    if (y < 0 || y >= kHeight || x < 0 || x >= view_.geom.width)
        return Part::None;
    const double cy = kHeight / 2.0;
    const Part parts[] = {Part::Close, Part::Minimize, Part::Maximize};
    for (int i = 0; i < 3; ++i) {
        const double cx = button_x(i, view_.geom.width) + kButton / 2;
        if (std::hypot(x - cx, y - cy) <= kButton / 2 + kHitPad)
            return parts[i];
    }
    return Part::Bar;
}

void Titlebar::set_hover(Part part) {
    if (hover_ != part) {
        hover_ = part;
        update();
    }
}

void Titlebar::set_pressed(Part part) {
    if (pressed_ != part) {
        pressed_ = part;
        update();
    }
}

void Titlebar::update() {
    const float scale = view_.output ? view_.output->wlr->scale : 1.0f;
    Drawn want;
    want.width = view_.geom.width;
    want.height = kHeight;
    want.scale = scale;
    want.title = view_.title();
    want.active = view_.activated;
    want.hover = hover_;
    want.pressed = pressed_;
    want.style = uint64_t(view_.server.config.corner_radius) << 2 | (view_.server.config.light ? 2 : 0) |
                 (view_.fullscreen ? 1 : 0);
    if (want == drawn_ || want.width <= 0)
        return;
    drawn_ = want;
    render(want.width, want.height, scale);
}

void Titlebar::render(int width, int height, float scale) {
    const int pw = int(std::ceil(width * scale));
    const int ph = int(std::ceil(height * scale));
    cairo_surface_t* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, pw, ph);
    cairo_t* cr = cairo_create(surface);
    cairo_scale(cr, scale, scale);

    const bool active = drawn_.active;
    const Chrome& c = view_.server.config.light ? kLight : kDark;

    // Bar. The window's top corners are rounded by the renderer, not here.
    set(cr, active ? c.bar_active : c.bar_inactive);
    cairo_paint(cr);
    set(cr, c.highlight);
    cairo_rectangle(cr, 0, 0, width, 1);
    cairo_fill(cr);
    set(cr, c.separator);
    cairo_rectangle(cr, 0, height - 1, width, 1);
    cairo_fill(cr);

    // Traffic lights. Glyphs show while the pointer is over any of them, as on macOS.
    const bool show_glyphs = hover_ == Part::Close || hover_ == Part::Minimize || hover_ == Part::Maximize;
    const Rgba colors[] = {kClose, kMinimize, kMaximize};
    const Part parts[] = {Part::Close, Part::Minimize, Part::Maximize};
    const double cy = height / 2.0;
    for (int i = 0; i < 3; ++i) {
        const double cx = button_x(i, width) + kButton / 2;
        const double r = kButton / 2;
        Rgba fill = (active || show_glyphs) ? colors[i] : c.button_inactive;
        if (pressed_ == parts[i])
            fill = darker(fill, 0.78);

        cairo_new_path(cr);
        cairo_arc(cr, cx, cy, r, 0, 2 * M_PI);
        set(cr, fill);
        cairo_fill_preserve(cr);
        // A hairline rim keeps the circles crisp against the bar.
        set(cr, darker(fill, 0.8));
        cairo_set_line_width(cr, 0.75);
        cairo_stroke(cr);

        if (!show_glyphs)
            continue;
        set(cr, kGlyph);
        cairo_set_line_width(cr, 1.3);
        cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
        const double g = 3.0;  // glyph half-size
        switch (parts[i]) {
        case Part::Close:
            cairo_move_to(cr, cx - g, cy - g);
            cairo_line_to(cr, cx + g, cy + g);
            cairo_move_to(cr, cx + g, cy - g);
            cairo_line_to(cr, cx - g, cy + g);
            cairo_stroke(cr);
            break;
        case Part::Minimize:
            cairo_move_to(cr, cx - g - 0.5, cy);
            cairo_line_to(cr, cx + g + 0.5, cy);
            cairo_stroke(cr);
            break;
        case Part::Maximize:
            // Two corner triangles, the macOS zoom glyph.
            cairo_move_to(cr, cx - g, cy + g - 0.2);
            cairo_line_to(cr, cx - g, cy - 0.6);
            cairo_line_to(cr, cx + 0.6, cy + g);
            cairo_close_path(cr);
            cairo_move_to(cr, cx + g, cy - g + 0.2);
            cairo_line_to(cr, cx + g, cy + 0.6);
            cairo_line_to(cr, cx - 0.6, cy - g);
            cairo_close_path(cr);
            cairo_fill(cr);
            break;
        default:
            break;
        }
    }

    // Title, centered on the bar; kept clear of the buttons on both sides so it
    // stays optically centered.
    const double reserve = kGroupWidth + 6;
    const double avail = width - 2 * reserve;
    if (avail > 20 && !drawn_.title.empty()) {
        PangoLayout* layout = pango_cairo_create_layout(cr);
        PangoFontDescription* font = pango_font_description_from_string("Sans Semi-Bold 10");
        pango_layout_set_font_description(layout, font);
        pango_font_description_free(font);
        pango_layout_set_text(layout, drawn_.title.c_str(), -1);
        pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
        pango_layout_set_width(layout, int(avail * PANGO_SCALE));
        pango_layout_set_single_paragraph_mode(layout, true);
        int tw, th;
        pango_layout_get_pixel_size(layout, &tw, &th);
        set(cr, active ? c.title_active : c.title_inactive);
        cairo_move_to(cr, std::round((width - tw) / 2.0), std::round((height - th) / 2.0));
        pango_cairo_show_layout(cr, layout);
        g_object_unref(layout);
    }

    cairo_destroy(cr);
    cairo_surface_flush(surface);

    wlr_buffer* old = held_;
    held_ = set_cairo_buffer(buffer_, surface, width, height);
    if (old)
        wlr_buffer_unlock(old);

    const int radius = view_.fullscreen ? 0 : view_.server.config.corner_radius;
    wlr_scene_buffer_set_corner_radii(buffer_, corner_radii_top(radius));
}

} // namespace atrium
