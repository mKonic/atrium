#include "panel_edge_core.hpp"

#include <algorithm>

namespace atrium::panel_edge {

Edge Tracker::update(const Pointer& p, double now_ms) {
    if (p.screen != screen_) {
        reached_ = pending_ = Edge::None;
        screen_ = p.screen;
    }
    if (p.screen < 0 || p.over == Over::AppFullscreen) {
        reached_ = pending_ = Edge::None;
        return reached_;
    }

    if (reached_ == Edge::Top && p.y >= p.top + std::max(1, p.top_band))
        reached_ = Edge::None;
    if (reached_ == Edge::Bottom && p.y < p.bottom - std::max(1, p.bottom_band))
        reached_ = Edge::None;
    if (reached_ != Edge::None) {
        pending_ = Edge::None;
        return reached_;
    }

    Edge at = Edge::None;
    if (p.y < p.top + 1)
        at = Edge::Top;
    else if (p.y >= p.bottom - 1)
        at = Edge::Bottom;
    if (at == Edge::None || p.blocked) {
        pending_ = Edge::None;
        return reached_;
    }
    if (at != pending_) {
        pending_ = at;
        since_ = now_ms;
        dwell_ = p.over == Over::UserFullscreen ? kFullscreenDwellMs : kDwellMs;
        return reached_;
    }
    if (now_ms - since_ >= dwell_) {
        reached_ = at;
        pending_ = Edge::None;
    }
    return reached_;
}

std::optional<double> Tracker::wake_in(double now_ms) const {
    if (pending_ == Edge::None)
        return std::nullopt;
    return std::max(1.0, since_ + dwell_ - now_ms);
}

} // namespace atrium::panel_edge
