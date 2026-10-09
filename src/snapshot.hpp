#pragma once
// A window as it was drawn, kept to animate: copies of its buffers (they
// hold the client's buffers locked), its shadow, outline and backing, each
// at its place relative to the window's frame. place() draws the whole into
// another box (scaled each way) at an opacity: zooming in on open, out on
// close, morphing between sizes, shrinking into the Dock. See snapshot_core.
#include "config.hpp"
#include "snapshot_core.hpp"
#include "wlr.hpp"

#include <functional>
#include <vector>

namespace atrium {

class Snapshot {
public:
    // Its tree under `parent`; `frame` is the window's frame in layout
    // coordinates, `root` the scene node the parts are found under (its
    // position is the frame's top left).
    Snapshot(wlr_scene_tree* parent, wlr_box frame);
    ~Snapshot();
    Snapshot(const Snapshot&) = delete;
    Snapshot& operator=(const Snapshot&) = delete;

    void add_buffers(wlr_scene_node* root);
    void add_shadow(const wlr_scene_shadow* shadow, const Color& color);
    void add_rect(const wlr_scene_rect* rect, const Color& straight_color);

    // The frame drawn into `to` (layout coordinates), at `alpha` of its own
    // opacity.
    void place(const FBox& to, float alpha);
    // Bent instead: each buffer drawn over a grid (kGenieCell), every point
    // of it where `at` takes it (both relative to the frame's top left).
    // The outline and backing are left out meanwhile, as they don't bend,
    // and the shadow too unless `keep_shadow` (then it stays flat). The
    // grid's cells are `cell_w` x `cell_h`. place() straightens it again.
    void warp(const std::function<FPoint(double, double)>& at, float alpha, double cell_w = kGenieCell,
              double cell_h = kGenieCell, bool keep_shadow = false);
    const wlr_box& frame() const { return frame_; }
    FBox frame_box() const { return {double(frame_.x), double(frame_.y), double(frame_.width), double(frame_.height)}; }
    wlr_scene_tree* tree() const { return tree_; }

private:
    enum class Kind { Buffer, Shadow, Rect };
    struct Part {
        Kind kind;
        wlr_scene_node* node;
        FBox box;  // relative to the frame's top left
        float opacity = 1;
        Color color{};  // shadow and rects: straight
        fx_corner_radii corners{};
        int radius = 0;  // the shadow's
        clipped_region clip{};
    };

    wlr_scene_tree* tree_;
    wlr_box frame_;
    std::vector<Part> parts_;
    bool warped_ = false;
};

} // namespace atrium
