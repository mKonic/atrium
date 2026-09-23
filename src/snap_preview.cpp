#include "snap_preview.hpp"

#include "server.hpp"
#include "space.hpp"

#include <cmath>

namespace atrium {

namespace {

constexpr float kFill[4] = {1.0f, 1.0f, 1.0f, 0.10f};
constexpr float kRing[4] = {1.0f, 1.0f, 1.0f, 0.30f};

int lerp(int a, int b, double t) {
    return int(std::lround(a + (b - a) * t));
}

} // namespace

SnapPreview::SnapPreview(Server& server) : server_(server) {
    tree_ = wlr_scene_tree_create(server.layer(Layer::Views));
    blur_ = wlr_scene_blur_create(tree_, 0, 0);
    wlr_scene_blur_set_should_only_blur_bottom_layer(blur_, false);
    constexpr float kClear[4] = {0, 0, 0, 0};
    fill_ = wlr_scene_rect_create(tree_, 0, 0, kClear);
    fill_->accepts_input = false;
    ring_ = wlr_scene_rect_create(tree_, 0, 0, kClear);
    ring_->accepts_input = false;
    wlr_scene_node_set_enabled(&tree_->node, false);
}

SnapPreview::~SnapPreview() {
    server_.animator.cancel_owner(this, false);
    wlr_scene_node_destroy(&tree_->node);
}

void SnapPreview::set_box(const wlr_box& b, float alpha) {
    box_ = b;
    alpha_ = alpha;
    const int r = server_.config.corner_radius;
    wlr_scene_node_set_position(&tree_->node, b.x, b.y);

    wlr_scene_blur_set_size(blur_, b.width, b.height);
    wlr_scene_blur_set_corner_radius(blur_, r);
    wlr_scene_blur_set_alpha(blur_, alpha);
    wlr_scene_node_set_enabled(&blur_->node, server_.config.blur);

    const float fa = kFill[3] * alpha, ra = kRing[3] * alpha;
    float fill[4] = {kFill[0] * fa, kFill[1] * fa, kFill[2] * fa, fa};  // premultiplied
    wlr_scene_rect_set_color(fill_, fill);
    wlr_scene_rect_set_size(fill_, b.width, b.height);
    wlr_scene_rect_set_corner_radius(fill_, r);

    float ring[4] = {kRing[0] * ra, kRing[1] * ra, kRing[2] * ra, ra};
    wlr_scene_rect_set_color(ring_, ring);
    wlr_scene_rect_set_size(ring_, b.width, b.height);
    wlr_scene_rect_set_corner_radius(ring_, r);
    wlr_scene_rect_set_clipped_region(ring_, clipped_region{
        .area = {1, 1, b.width - 2, b.height - 2},
        .corners = corner_radii_all(r > 0 ? r - 1 : 0),
    });
}

void SnapPreview::show(const wlr_box& target, wlr_scene_node* below, const wlr_box& from) {
    if (visible_ && wlr_box_equal(&target, &box_) && alpha_ >= 1.0f)
        return;
    server_.animator.cancel_owner(this, false);
    if (below && below->parent) {
        wlr_scene_node_reparent(&tree_->node, below->parent);
        wlr_scene_node_place_below(&tree_->node, below);
    }
    wlr_scene_node_set_enabled(&tree_->node, true);
    // Grow out of the window when appearing; glide from the old zone otherwise.
    const wlr_box start = visible_ ? box_ : from;
    const float a0 = visible_ ? alpha_ : 0.0f;
    visible_ = true;
    server_.animator.start(this, 180, Ease::OutQuint, [this, start, target, a0](double t) {
        set_box({lerp(start.x, target.x, t), lerp(start.y, target.y, t), lerp(start.width, target.width, t),
                 lerp(start.height, target.height, t)},
                float(a0 + (1.0f - a0) * t));
    });
}

void SnapPreview::hide() {
    if (!visible_)
        return;
    visible_ = false;
    server_.animator.cancel_owner(this, false);
    const float a0 = alpha_;
    const wlr_box b = box_;
    server_.animator.start(this, 120, Ease::InCubic, [this, a0, b](double t) {
        set_box(b, float(a0 * (1 - t)));
    }, [this] {
        if (visible_)
            return;
        wlr_scene_node_set_enabled(&tree_->node, false);
        wlr_scene_node_reparent(&tree_->node, server_.layer(Layer::Views));
    });
}

void SnapPreview::rescue(Space* space) {
    if (tree_->node.parent == space->tree || tree_->node.parent == space->fullscreen_tree) {
        server_.animator.cancel_owner(this, false);
        visible_ = false;
        wlr_scene_node_set_enabled(&tree_->node, false);
        wlr_scene_node_reparent(&tree_->node, server_.layer(Layer::Views));
    }
}

} // namespace atrium
