#include "window_copy.hpp"

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

struct CopyCtx {
    scene::Tree* into;
    int ox, oy;  // the root's own position, which for_each_buffer counts
    std::vector<scene::Buffer*> nodes;
    std::vector<std::array<int, 4>> boxes;
    std::vector<scene::Radii> corners;
};

void copy_buffer(scene::Buffer* src, int sx, int sy, void* data) {
    auto* c = static_cast<CopyCtx*>(data);
    if (!src->buffer)
        return;
    scene::Buffer* dst = scene::Buffer::create(c->into, src->buffer);
    dst->set_source_box(&src->src_box);
    dst->set_transform(src->transform);
    int w = src->dst_width, h = src->dst_height;
    if (w <= 0 || h <= 0) {
        w = src->buffer->width;
        h = src->buffer->height;
    }
    c->nodes.push_back(dst);
    c->boxes.push_back({sx - c->ox, sy - c->oy, w, h});
    c->corners.push_back(src->corners);
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
    if (inner_)
        inner_->destroy();
    inner_ = scene::Tree::create(tree_);
    pieces_.clear();
    if (!view_.tree)
        return;
    CopyCtx ctx{inner_, view_.tree->x, view_.tree->y, {}, {}, {}};
    view_.tree->for_each_buffer([&](scene::Buffer* b_, int x_, int y_) { (copy_buffer)(b_, x_, y_, &ctx); });
    for (size_t i = 0; i < ctx.nodes.size(); ++i)
        pieces_.push_back({ctx.nodes[i], ctx.boxes[i][0], ctx.boxes[i][1], ctx.boxes[i][2], ctx.boxes[i][3],
                           ctx.corners[i]});
    if (width_ > 0)
        place(width_, height_);
}

void WindowCopy::place(int width, int height) {
    width_ = width;
    height_ = height;
    const double sx = width / double(std::max(1, view_.geom.width));
    const double sy = height / double(std::max(1, view_.geom.height));
    for (Piece& p : pieces_) {
        p.node->set_position(round_i(p.x * sx), round_i(p.y * sy));
        p.node->set_dest_size(std::max(1, round_i(p.w * sx)), std::max(1, round_i(p.h * sy)));
        p.node->set_corner_radii(scene::Radii(scaled(p.corners.tl, sx), scaled(p.corners.tr, sx), scaled(p.corners.br, sx), scaled(p.corners.bl, sx)));
    }
}

} // namespace atrium
