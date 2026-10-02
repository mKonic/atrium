#pragma once
#include "util/box.hpp"
#include <cstdint>

namespace atrium::wl {


// xdg_positioner's rules: where a popup goes relative to its parent, and
// what it may do when that doesn't fit (flip, slide, resize). The geometry
// and the unconstraining follow wlroots (types/xdg_shell/wlr_xdg_positioner.c).
struct PositionerRules {
    // Edge bits, the xdg_positioner.anchor and .gravity enums' values.
    enum Anchor : uint32_t { None, Top, Bottom, Left, Right, TopLeft, BottomLeft, TopRight, BottomRight };
    enum Adjust : uint32_t { SlideX = 1, SlideY = 2, FlipX = 4, FlipY = 8, ResizeX = 16, ResizeY = 32 };

    Box anchor_rect;
    int width = 0, height = 0;
    uint32_t anchor = None, gravity = None;
    uint32_t constraint_adjustment = 0;
    int offset_x = 0, offset_y = 0;
    bool reactive = false;
    bool has_parent_size = false;
    int parent_width = 0, parent_height = 0;
    bool has_parent_configure = false;
    uint32_t parent_configure_serial = 0;

    bool complete() const { return width > 0 && anchor_rect.width > 0; }
    // Where the popup goes, in its parent's window geometry, ignoring limits.
    Box geometry() const;
    // `box` moved (or shrunk) to fit inside `constraint` as far as the rules
    // allow: flip, then slide, then resize.
    void unconstrain(const Box& constraint, Box& box) const;
};

} // namespace atrium::wl
