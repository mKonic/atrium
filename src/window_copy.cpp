#include "window_copy.hpp"

#include "geometry.hpp"
#include "view.hpp"

#include <algorithm>
#include <cmath>

namespace atrium {

namespace {

int round_i(double v) {
    return int(std::lround(v));
}

uint16_t scaled(uint16_t r, double s) {
    return uint16_t(std::max(0L, std::lround(r * s)));
}

} // namespace

WindowCopy::WindowCopy(View& view, scene::Tree* parent) : view_(view) {
    tree_ = scene::Tree::create(parent);
    refresh();
}

WindowCopy::~WindowCopy() {
    tree_->destroy();
}

void WindowCopy::refresh() {
    struct Source {
        scene::Buffer* buffer;
        int x, y, w, h;
    };
    std::vector<Source> sources;
    if (view_.tree) {
        const int ox = view_.tree->x, oy = view_.tree->y;  // for_each_buffer counts the root's position
        view_.tree->for_each_buffer([&](scene::Buffer* b, int sx, int sy) {
            if (!b->buffer)
                return;
            int w = b->dst_width, h = b->dst_height;
            if (w <= 0 || h <= 0) {
                w = b->buffer->width;
                h = b->buffer->height;
            }
            sources.push_back({b, sx - ox, sy - oy, w, h});
        });
    }

    // Same pieces as before (a video playing): the copies take the new
    // buffers in place, rather than being torn down and made again for
    // every frame. Otherwise they are made again.
    if (!inner_ || sources.size() != pieces_.size()) {
        if (inner_)
            inner_->destroy();
        inner_ = scene::Tree::create(tree_);
        pieces_.clear();
        for (const Source& src : sources)
            pieces_.push_back({scene::Buffer::create(inner_, src.buffer->buffer), 0, 0, 0, 0, {}});
    }
    frame_w_ = std::max(1, view_.geom.width);
    frame_h_ = std::max(1, view_.geom.height);
    for (size_t i = 0; i < sources.size(); ++i) {
        const Source& src = sources[i];
        Piece& p = pieces_[i];
        if (p.node->buffer != src.buffer->buffer)
            p.node->set_buffer(src.buffer->buffer);
        p.src = src.buffer->src_box;
        p.node->set_source_box(&p.src);
        p.node->set_transform(src.buffer->transform);
        p.x = src.x;
        p.y = src.y;
        p.w = src.w;
        p.h = src.h;
        p.corners = src.buffer->corners;
    }
    if (width_ > 0)
        place(width_, height_);
}

void WindowCopy::place(int width, int height) {
    width_ = width;
    height_ = height;
    const int fw = frame_w_, fh = frame_h_;
    const double sx = width / double(fw);
    const double sy = height / double(fh);
    for (Piece& p : pieces_) {
        // Only the window itself: what an app draws past its frame (its own
        // shadow) would be a dark rim around the miniature.
        const bool plain = p.node->transform == WL_OUTPUT_TRANSFORM_NORMAL && p.node->buffer;
        const auto c = geometry::clip_to_frame({p.x, p.y, p.w, p.h}, p.src, plain ? p.node->buffer->width : 0,
                                               plain ? p.node->buffer->height : 0, fw, fh);
        if (!c) {
            p.node->set_enabled(false);
            continue;
        }
        p.node->set_enabled(true);
        // A rotated buffer's crop isn't worked out: it shows whole.
        const Box b = plain ? c->box : Box{p.x, p.y, p.w, p.h};
        if (plain)
            p.node->set_source_box(&c->src);
        p.node->set_position(round_i(b.x * sx), round_i(b.y * sy));
        p.node->set_dest_size(std::max(1, round_i(b.width * sx)), std::max(1, round_i(b.height * sy)));
        p.node->set_corner_radii(scene::Radii(scaled(p.corners.tl, sx), scaled(p.corners.tr, sx), scaled(p.corners.br, sx), scaled(p.corners.bl, sx)));
    }
}

} // namespace atrium
