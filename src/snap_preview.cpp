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
    tree_ = scene::Tree::create(server.layer(Layer::Views));
    blur_ = scene::Blur::create(tree_, 0, 0);
    blur_->set_use_cache(false);
    constexpr float kClear[4] = {0, 0, 0, 0};
    fill_ = scene::Rect::create(tree_, 0, 0, kClear);
    fill_->accepts_input = false;
    ring_ = scene::Rect::create(tree_, 0, 0, kClear);
    ring_->accepts_input = false;
    tree_->set_enabled(false);
}

SnapPreview::~SnapPreview() {
    server_.animator.cancel_owner(this, false);
    tree_->destroy();
}

void SnapPreview::set_box(const Box& b, float alpha) {
    box_ = b;
    alpha_ = alpha;
    const int r = server_.config.corner_radius;
    tree_->set_position(b.x, b.y);

    blur_->set_size(b.width, b.height);
    blur_->set_corner_radius(r);
    blur_->set_alpha(alpha);
    blur_->set_enabled(server_.config.blur);

    const float fa = kFill[3] * alpha, ra = kRing[3] * alpha;
    float fill[4] = {kFill[0] * fa, kFill[1] * fa, kFill[2] * fa, fa};  // premultiplied
    fill_->set_color(fill);
    fill_->set_size(b.width, b.height);
    fill_->set_corner_radius(r);

    float ring[4] = {kRing[0] * ra, kRing[1] * ra, kRing[2] * ra, ra};
    ring_->set_color(ring);
    ring_->set_size(b.width, b.height);
    ring_->set_corner_radius(r);
    ring_->set_cut_out(scene::CutOut{
        .area = {1, 1, b.width - 2, b.height - 2},
        .corners = scene::Radii::all(r > 0 ? r - 1 : 0),
    });
}

void SnapPreview::show(const Box& target, scene::Node* below, const Box& from) {
    if (visible_ && box_equal(&target, &box_) && alpha_ >= 1.0f)
        return;
    server_.animator.cancel_owner(this, false);
    if (below && below->parent) {
        tree_->reparent(below->parent);
        tree_->place_below(below);
    }
    tree_->set_enabled(true);
    // Grow out of the window when appearing; glide from the old zone otherwise.
    const Box start = visible_ ? box_ : from;
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
    const Box b = box_;
    server_.animator.start(this, 120, Ease::InCubic, [this, a0, b](double t) {
        set_box(b, float(a0 * (1 - t)));
    }, [this] {
        if (visible_)
            return;
        tree_->set_enabled(false);
        tree_->reparent(server_.layer(Layer::Views));
    });
}

void SnapPreview::rescue(Space* space) {
    if (tree_->parent == space->tree || tree_->parent == space->fullscreen_tree) {
        server_.animator.cancel_owner(this, false);
        visible_ = false;
        tree_->set_enabled(false);
        tree_->reparent(server_.layer(Layer::Views));
    }
}

} // namespace atrium
