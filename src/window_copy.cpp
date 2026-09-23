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
    wlr_scene_tree* into;
    int ox, oy;  // the root's own position, which for_each_buffer counts
    std::vector<wlr_scene_buffer*> nodes;
    std::vector<std::array<int, 4>> boxes;
    std::vector<fx_corner_radii> corners;
};

void copy_buffer(wlr_scene_buffer* src, int sx, int sy, void* data) {
    auto* c = static_cast<CopyCtx*>(data);
    if (!src->buffer)
        return;
    wlr_scene_buffer* dst = wlr_scene_buffer_create(c->into, src->buffer);
    wlr_scene_buffer_set_source_box(dst, &src->src_box);
    wlr_scene_buffer_set_transform(dst, src->transform);
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

WindowCopy::WindowCopy(View& view, wlr_scene_tree* parent) : view_(view) {
    tree_ = wlr_scene_tree_create(parent);
    refresh();
}

WindowCopy::~WindowCopy() {
    wlr_scene_node_destroy(&tree_->node);
}

void WindowCopy::refresh() {
    if (inner_)
        wlr_scene_node_destroy(&inner_->node);
    inner_ = wlr_scene_tree_create(tree_);
    pieces_.clear();
    if (!view_.tree)
        return;
    CopyCtx ctx{inner_, view_.tree->node.x, view_.tree->node.y, {}, {}, {}};
    wlr_scene_node_for_each_buffer(&view_.tree->node, copy_buffer, &ctx);
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
        wlr_scene_node_set_position(&p.node->node, round_i(p.x * sx), round_i(p.y * sy));
        wlr_scene_buffer_set_dest_size(p.node, std::max(1, round_i(p.w * sx)), std::max(1, round_i(p.h * sy)));
        wlr_scene_buffer_set_corner_radii(p.node, corner_radii_new(
            scaled(p.corners.top_left, sx), scaled(p.corners.top_right, sx),
            scaled(p.corners.bottom_right, sx), scaled(p.corners.bottom_left, sx)));
    }
}

} // namespace atrium
