#include "snapshot_core.hpp"

#include <algorithm>
#include <cmath>

namespace atrium {

FBox snapshot_map(const FBox& part, double frame_w, double frame_h, const FBox& to) {
    const double sx = frame_w > 0 ? to.width / frame_w : 1, sy = frame_h > 0 ? to.height / frame_h : 1;
    return {to.x + part.x * sx, to.y + part.y * sy, part.width * sx, part.height * sy};
}

FBox popin_box(const FBox& goal, double fraction) {
    const double w = std::clamp(goal.width * fraction, std::min(5.0, goal.width), goal.width);
    const double h = std::clamp(goal.height * fraction, std::min(5.0, goal.height), goal.height);
    return {goal.x + (goal.width - w) / 2, goal.y + (goal.height - h) / 2, w, h};
}

FBox lerp(const FBox& a, const FBox& b, double t) {
    auto mix = [t](double p, double q) { return p + (q - p) * t; };
    return {mix(a.x, b.x), mix(a.y, b.y), mix(a.width, b.width), mix(a.height, b.height)};
}

GenieEdge genie_edge(const FBox& screen, const FBox& dock, const FBox& icon) {
    if (dock.width >= dock.height)
        return icon.y + icon.height / 2 <= screen.y + screen.height / 2 ? GenieEdge::Top : GenieEdge::Bottom;
    return icon.x + icon.width / 2 <= screen.x + screen.width / 2 ? GenieEdge::Left : GenieEdge::Right;
}

// magiclamp.cpp's apply(), a point at a time (each of its quads' corners is
// worked out from its own row, or column, alone).
FPoint genie_point(GenieEdge edge, const FBox& geo, const FBox& icon, double progress, double qx, double qy) {
    const double W = geo.width, H = geo.height;
    // Across: towards the same fraction of the icon.
    auto toward_x = [&](double pp) { return (icon.x + icon.width * (qx / W) - (qx + geo.x)) * pp + qx; };
    auto toward_y = [&](double pp) { return (icon.y + icon.height * (qy / H) - (qy + geo.y)) * pp + qy; };
    switch (edge) {
    case GenieEdge::Bottom: {
        const double f = qy + (H - qy) * progress;
        const double offset = (icon.y + qy - geo.y) * progress * (f * f * f / (H * H * H));
        const double pp = std::abs(std::min(offset / (icon.y - geo.y - qy), 1.0));
        return {toward_x(pp), std::min(icon.y - geo.y, qy + offset)};
    }
    case GenieEdge::Top: {
        const double f = H - qy + qy * progress;
        const double offset = (geo.y - icon.height + H + qy - icon.y) * progress * (f * f * f / (H * H * H));
        const double pp = std::abs(std::min(offset / (geo.y - icon.height + H - icon.y - (H - qy)), 1.0));
        return {toward_x(pp), std::max(icon.y + icon.height - geo.y, qy - offset)};
    }
    case GenieEdge::Left: {
        const double f = W - qx + qx * progress;
        const double offset = (geo.x - icon.width + W + qx - icon.x) * progress * (f * f * f / (W * W * W));
        const double pp = std::abs(std::min(offset / (geo.x - icon.width + W - icon.x - (W - qx)), 1.0));
        return {std::max(icon.x + icon.width - geo.x, qx - offset), toward_y(pp)};
    }
    case GenieEdge::Right: {
        const double f = qx + (W - qx) * progress;
        const double offset = (icon.x + qx - geo.x) * progress * (f * f * f / (W * W * W));
        const double pp = std::abs(std::min(offset / (icon.x - geo.x - qx), 1.0));
        return {std::min(icon.x - geo.x, qx + offset), toward_y(pp)};
    }
    }
    return {qx, qy};
}

} // namespace atrium
