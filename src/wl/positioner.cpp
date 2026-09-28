// The geometry and constraint handling follow wlroots'
// types/xdg_shell/wlr_xdg_positioner.c (MIT).
#include "wl/positioner.hpp"

#include <algorithm>
#include <cstdlib>

namespace atrium::wl {

namespace {

enum Edge : uint32_t { EdgeNone = 0, EdgeTop = 1, EdgeBottom = 2, EdgeLeft = 4, EdgeRight = 8 };

uint32_t edges_of(uint32_t anchor) {
    switch (anchor) {
    case PositionerRules::Top:
        return EdgeTop;
    case PositionerRules::TopLeft:
        return EdgeTop | EdgeLeft;
    case PositionerRules::TopRight:
        return EdgeTop | EdgeRight;
    case PositionerRules::Bottom:
        return EdgeBottom;
    case PositionerRules::BottomLeft:
        return EdgeBottom | EdgeLeft;
    case PositionerRules::BottomRight:
        return EdgeBottom | EdgeRight;
    case PositionerRules::Left:
        return EdgeLeft;
    case PositionerRules::Right:
        return EdgeRight;
    default:
        return EdgeNone;
    }
}

uint32_t invert_x(uint32_t a) {
    switch (a) {
    case PositionerRules::Left:
        return PositionerRules::Right;
    case PositionerRules::Right:
        return PositionerRules::Left;
    case PositionerRules::TopLeft:
        return PositionerRules::TopRight;
    case PositionerRules::TopRight:
        return PositionerRules::TopLeft;
    case PositionerRules::BottomLeft:
        return PositionerRules::BottomRight;
    case PositionerRules::BottomRight:
        return PositionerRules::BottomLeft;
    default:
        return a;
    }
}

uint32_t invert_y(uint32_t a) {
    switch (a) {
    case PositionerRules::Top:
        return PositionerRules::Bottom;
    case PositionerRules::Bottom:
        return PositionerRules::Top;
    case PositionerRules::TopLeft:
        return PositionerRules::BottomLeft;
    case PositionerRules::BottomLeft:
        return PositionerRules::TopLeft;
    case PositionerRules::TopRight:
        return PositionerRules::BottomRight;
    case PositionerRules::BottomRight:
        return PositionerRules::TopRight;
    default:
        return a;
    }
}

// Each edge's distance past the constraint: positive is outside.
struct Offsets {
    int top, bottom, left, right;
    bool fits() const { return top <= 0 && bottom <= 0 && left <= 0 && right <= 0; }
};

Offsets offsets_of(const Box& c, const Box& b) {
    return {c.y - b.y, b.y + b.height - c.y - c.height, c.x - b.x, b.x + b.width - c.x - c.width};
}

bool by_flip(const PositionerRules& r, const Box& c, Box& box, Offsets& o) {
    // Flipping helps only when exactly one edge on an axis is out.
    const bool flip_x = ((o.left > 0) ^ (o.right > 0)) && (r.constraint_adjustment & PositionerRules::FlipX);
    const bool flip_y = ((o.top > 0) ^ (o.bottom > 0)) && (r.constraint_adjustment & PositionerRules::FlipY);
    if (!flip_x && !flip_y)
        return false;
    PositionerRules flipped = r;
    if (flip_x) {
        flipped.anchor = invert_x(flipped.anchor);
        flipped.gravity = invert_x(flipped.gravity);
    }
    if (flip_y) {
        flipped.anchor = invert_y(flipped.anchor);
        flipped.gravity = invert_y(flipped.gravity);
    }
    const Box fb = flipped.geometry();
    const Offsets fo = offsets_of(c, fb);
    // Only where it helps.
    if (fo.left <= 0 && fo.right <= 0) {
        box.x = fb.x;
        o.left = fo.left;
        o.right = fo.right;
    }
    if (fo.top <= 0 && fo.bottom <= 0) {
        box.y = fb.y;
        o.top = fo.top;
        o.bottom = fo.bottom;
    }
    return o.fits();
}

bool by_slide(const PositionerRules& r, const Box& c, Box& box, Offsets& o) {
    const uint32_t gravity = edges_of(r.gravity);
    const bool slide_x = (o.left > 0 || o.right > 0) && (r.constraint_adjustment & PositionerRules::SlideX);
    const bool slide_y = (o.top > 0 || o.bottom > 0) && (r.constraint_adjustment & PositionerRules::SlideY);
    if (!slide_x && !slide_y)
        return false;
    if (slide_x) {
        if (o.left > 0 && o.right > 0) {
            // Bigger than the constraint: towards the gravity (or right,
            // as GTK does on X11, when there is none).
            if (gravity & EdgeLeft)
                box.x -= o.right;
            else
                box.x += o.left;
        } else if (std::abs(o.left) < std::abs(o.right)) {
            box.x += o.left;
        } else {
            box.x -= o.right;
        }
    }
    if (slide_y) {
        if (o.top > 0 && o.bottom > 0) {
            if (gravity & EdgeTop)
                box.y -= o.bottom;
            else
                box.y += o.top;
        } else if (std::abs(o.top) < std::abs(o.bottom)) {
            box.y += o.top;
        } else {
            box.y -= o.bottom;
        }
    }
    o = offsets_of(c, box);
    return o.fits();
}

bool by_resize(const PositionerRules& r, const Box& c, Box& box, Offsets& o) {
    const bool resize_x = (o.left > 0 || o.right > 0) && (r.constraint_adjustment & PositionerRules::ResizeX);
    const bool resize_y = (o.top > 0 || o.bottom > 0) && (r.constraint_adjustment & PositionerRules::ResizeY);
    if (!resize_x && !resize_y)
        return false;
    Offsets clipped{std::max(o.top, 0), std::max(o.bottom, 0), std::max(o.left, 0), std::max(o.right, 0)};
    Box resized = box;
    if (resize_x) {
        resized.x += clipped.left;
        resized.width -= clipped.left + clipped.right;
    }
    if (resize_y) {
        resized.y += clipped.top;
        resized.height -= clipped.top + clipped.bottom;
    }
    if (resized.empty())
        return false;
    box = resized;
    o = offsets_of(c, box);
    return o.fits();
}

} // namespace

Box PositionerRules::geometry() const {
    Box b{offset_x, offset_y, width, height};
    uint32_t e = edges_of(anchor);
    if (e & EdgeTop)
        b.y += anchor_rect.y;
    else if (e & EdgeBottom)
        b.y += anchor_rect.y + anchor_rect.height;
    else
        b.y += anchor_rect.y + anchor_rect.height / 2;
    if (e & EdgeLeft)
        b.x += anchor_rect.x;
    else if (e & EdgeRight)
        b.x += anchor_rect.x + anchor_rect.width;
    else
        b.x += anchor_rect.x + anchor_rect.width / 2;

    e = edges_of(gravity);
    if (e & EdgeTop)
        b.y -= b.height;
    else if (~e & EdgeBottom)
        b.y -= b.height / 2;
    if (e & EdgeLeft)
        b.x -= b.width;
    else if (~e & EdgeRight)
        b.x -= b.width / 2;
    return b;
}

void PositionerRules::unconstrain(const Box& constraint, Box& box) const {
    Offsets o = offsets_of(constraint, box);
    if (o.fits())
        return;
    if (by_flip(*this, constraint, box, o))
        return;
    if (by_slide(*this, constraint, box, o))
        return;
    by_resize(*this, constraint, box, o);
}

} // namespace atrium::wl
