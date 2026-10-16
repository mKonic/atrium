#include "launch_feedback.hpp"

#include "cairo_buffer.hpp"
#include "output.hpp"
#include "seat.hpp"
#include "server.hpp"

#include <cairo.h>

#include <algorithm>
#include <chrono>
#include <cmath>

namespace atrium {

namespace {

// KWin's startupfeedback: one bounce lasts 500 ms, a startup 5 seconds.
constexpr int kBounceMs = 500;
constexpr long long kTimeoutMs = 5000;

long long now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

} // namespace

LaunchFeedback::LaunchFeedback(Server& server) : server_(server) {
    timer_ = wl_event_loop_add_timer(server.loop, [](void* data) {
        static_cast<LaunchFeedback*>(data)->tick();
        return 0;
    }, this);
}

LaunchFeedback::~LaunchFeedback() {
    if (timer_)
        wl_event_source_remove(timer_);
    stop();
}

void LaunchFeedback::start(const std::string& token, const std::string& app_id, const std::string& icon) {
    // KWin shows none while the pointer is hidden (typing, a game's own).
    startups_.push_back({token, app_id, icon, now_ms()});
    show();
}

void LaunchFeedback::finish_token(const std::string& token) {
    std::erase_if(startups_, [&](const Startup& s) { return s.token == token; });
    show();
}

void LaunchFeedback::finish_app(const std::string& app_id) {
    std::erase_if(startups_, [&](const Startup& s) { return !app_id.empty() && s.app_id == app_id; });
    show();
}

void LaunchFeedback::window_opened() {
    startups_.clear();
    show();
}

// The latest startup's icon, at KWin's size: the cursor size over 1.5,
// grown by the bounce ratio (icon size over 16).
void LaunchFeedback::show() {
    if (startups_.empty()) {
        stop();
        return;
    }
    const Startup& s = startups_.back();
    if (tree_ && shown_icon_ == s.icon) {
        tick();
        return;
    }
    stop();
    cairo_surface_t* png = s.icon.empty() ? nullptr : cairo_image_surface_create_from_png(s.icon.c_str());
    if (png && cairo_surface_status(png) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(png);
        png = nullptr;
    }
    const int base = std::max(16, int(server_.config.cursor_size / 1.5));
    icon_size_ = int(base * (base / 16.0));
    const double scale = server_.focused_output ? server_.focused_output->wlr->scale : 1.0;
    const int px = int(std::ceil(icon_size_ * scale));
    cairo_surface_t* img = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, px, px);
    cairo_t* cr = cairo_create(img);
    if (png) {
        const int w = cairo_image_surface_get_width(png), h = cairo_image_surface_get_height(png);
        const double k = double(px) / std::max(w, h);
        cairo_scale(cr, k, k);
        cairo_set_source_surface(cr, png, (std::max(w, h) - w) / 2.0, (std::max(w, h) - h) / 2.0);
        cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
        cairo_paint(cr);
        cairo_surface_destroy(png);
    } else {
        // No icon: KWin falls back to system-run; a plain dot does here.
        cairo_arc(cr, px / 2.0, px / 2.0, px / 3.0, 0, 2 * M_PI);
        cairo_set_source_rgba(cr, 0.5, 0.5, 0.5, 0.9);
        cairo_fill(cr);
    }
    cairo_destroy(cr);
    cairo_surface_flush(img);
    tree_ = wlr_scene_tree_create(server_.layer(Layer::InputPopup));
    node_ = wlr_scene_buffer_create(tree_, nullptr);
    buffer_ = set_cairo_buffer(node_, img, icon_size_, icon_size_);
    shown_icon_ = s.icon;
    phase_start_ms_ = now_ms();
    tick();
}

void LaunchFeedback::stop() {
    if (tree_)
        wlr_scene_node_destroy(&tree_->node);
    tree_ = nullptr;
    node_ = nullptr;
    if (buffer_)
        wlr_buffer_unlock(buffer_);
    buffer_ = nullptr;
    shown_icon_.clear();
    if (timer_)
        wl_event_source_timer_update(timer_, 0);
}

void LaunchFeedback::tick() {
    const long long now = now_ms();
    std::erase_if(startups_, [&](const Startup& s) { return now - s.started_ms > kTimeoutMs; });
    if (startups_.empty()) {
        stop();
        return;
    }
    if (!tree_) {
        show();
        return;
    }
    // Beside the pointer (KWin: 16 + 7 for cursors up to 32), kept on its
    // screen, bouncing: sin((t + 1)π) * 24 + 8 over a 500 ms bounce.
    const int cursor = server_.config.cursor_size;
    const int offset = cursor <= 16 ? 15 : cursor <= 32 ? 23 : cursor <= 48 ? 31 : 39;
    const double t = double((now - phase_start_ms_) % kBounceMs) / kBounceMs;
    const double bounce = std::sin((t + 1) * M_PI) * 24 + 8;
    double x = server_.seat->cursor->x + offset, y = server_.seat->cursor->y + offset;
    if (Output* o = server_.output_at(server_.seat->cursor->x, server_.seat->cursor->y)) {
        const double margin = offset / 4.0;
        x = std::clamp(x, o->box.x + margin, o->box.x + o->box.width - margin - icon_size_);
        y = std::clamp(y, o->box.y + margin, o->box.y + o->box.height - margin - icon_size_);
    }
    // Squeezed at the bottom of a bounce, as KWin draws it.
    const double squeeze = std::pow(std::cos((t - 0.25) * M_PI), 2) * 24 - 12;
    const int w = int(icon_size_ * (64 + squeeze) / 64), h = int(icon_size_ * (64 - squeeze) / 64);
    wlr_scene_buffer_set_dest_size(node_, w, h);
    wlr_scene_node_set_position(&tree_->node, int(x - (w - icon_size_) / 2.0), int(y + bounce - (h - icon_size_)));
    wlr_scene_node_raise_to_top(&tree_->node);
    wl_event_source_timer_update(timer_, 16);
}

} // namespace atrium
