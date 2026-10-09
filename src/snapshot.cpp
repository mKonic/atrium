#include "snapshot.hpp"

#include <algorithm>
#include <cmath>

namespace atrium {

namespace {

int round(double v) {
    return int(std::lround(v));
}

fx_corner_radii scaled(const fx_corner_radii& c, double s) {
    auto r = [s](uint16_t v) { return uint16_t(std::max(0L, std::lround(v * s))); };
    return {r(c.top_left), r(c.top_right), r(c.bottom_right), r(c.bottom_left)};
}

clipped_region scaled(const clipped_region& c, double sx, double sy) {
    clipped_region out = c;
    out.area = {round(c.area.x * sx), round(c.area.y * sy), round(c.area.width * sx), round(c.area.height * sy)};
    out.corners = scaled(c.corners, std::min(sx, sy));
    return out;
}

struct BufferWalk {
    Snapshot* self;
    wlr_scene_tree* tree;
    int origin_x, origin_y;  // the root's own position: for_each_buffer counts it
    std::vector<std::pair<wlr_scene_buffer*, wlr_scene_buffer*>>* out;
};

} // namespace

Snapshot::Snapshot(wlr_scene_tree* parent, wlr_box frame) : tree_(wlr_scene_tree_create(parent)), frame_(frame) {}

Snapshot::~Snapshot() {
    wlr_scene_node_destroy(&tree_->node);
}

void Snapshot::add_buffers(wlr_scene_node* root) {
    std::vector<std::pair<wlr_scene_buffer*, wlr_scene_buffer*>> made;
    BufferWalk walk{this, tree_, root->x, root->y, &made};
    wlr_scene_node_for_each_buffer(root, [](wlr_scene_buffer* src, int sx, int sy, void* data) {
        auto* w = static_cast<BufferWalk*>(data);
        if (!src->buffer)
            return;
        wlr_scene_buffer* dst = wlr_scene_buffer_create(w->tree, src->buffer);
        wlr_scene_buffer_set_source_box(dst, &src->src_box);
        wlr_scene_buffer_set_transform(dst, src->transform);
        // Where it was, before anything is placed.
        wlr_scene_node_set_position(&dst->node, sx - w->origin_x, sy - w->origin_y);
        w->out->emplace_back(src, dst);
    }, &walk);
    for (auto [src, dst] : made) {
        const int w = src->dst_width > 0 ? src->dst_width : src->buffer->width;
        const int h = src->dst_height > 0 ? src->dst_height : src->buffer->height;
        Part p{Kind::Buffer, &dst->node, {double(dst->node.x), double(dst->node.y), double(w), double(h)}};
        p.opacity = src->opacity;
        p.corners = src->corners;
        parts_.push_back(p);
    }
}

void Snapshot::add_shadow(const wlr_scene_shadow* s, const Color& color) {
    if (!s || !s->node.enabled)
        return;
    wlr_scene_shadow* dst = wlr_scene_shadow_create(tree_, s->width, s->height, s->corner_radius, s->blur_sigma,
                                                    color.data());
    // Under the buffers, as on the window.
    wlr_scene_node_lower_to_bottom(&dst->node);
    Part p{Kind::Shadow, &dst->node, {double(s->node.x), double(s->node.y), double(s->width), double(s->height)}};
    p.color = color;
    p.radius = s->corner_radius;
    p.clip = s->clipped_region;
    parts_.push_back(p);
}

void Snapshot::add_rect(const wlr_scene_rect* r, const Color& color) {
    if (!r || !r->node.enabled)
        return;
    wlr_scene_rect* dst = wlr_scene_rect_create(tree_, r->width, r->height, premultiplied(color).data());
    dst->accepts_input = false;
    Part p{Kind::Rect, &dst->node, {double(r->node.x), double(r->node.y), double(r->width), double(r->height)}};
    p.color = color;
    p.corners = r->corners;
    p.clip = r->clipped_region;
    parts_.push_back(p);
}

void Snapshot::place(const FBox& to, float alpha) {
    if (warped_) {
        warped_ = false;
        for (Part& p : parts_) {
            wlr_scene_node_set_enabled(p.node, true);
            if (p.kind == Kind::Buffer)
                wlr_scene_buffer_set_warp(wlr_scene_buffer_from_node(p.node), 0, 0, nullptr, 0, 0);
        }
    }
    const double fw = frame_.width, fh = frame_.height;
    const double sx = fw > 0 ? to.width / fw : 1, sy = fh > 0 ? to.height / fh : 1;
    for (Part& p : parts_) {
        const FBox b = snapshot_map(p.box, fw, fh, to);
        const int x = round(b.x), y = round(b.y);
        const int w = std::max(1, round(b.x + b.width) - x), h = std::max(1, round(b.y + b.height) - y);
        wlr_scene_node_set_position(p.node, x, y);
        switch (p.kind) {
        case Kind::Buffer: {
            auto* buf = wlr_scene_buffer_from_node(p.node);
            wlr_scene_buffer_set_dest_size(buf, w, h);
            wlr_scene_buffer_set_opacity(buf, p.opacity * alpha);
            wlr_scene_buffer_set_corner_radii(buf, scaled(p.corners, std::min(sx, sy)));
            break;
        }
        case Kind::Shadow: {
            auto* s = wlr_scene_shadow_from_node(p.node);
            wlr_scene_shadow_set_size(s, w, h);
            wlr_scene_shadow_set_corner_radius(s, round(p.radius * std::min(sx, sy)));
            wlr_scene_shadow_set_clipped_region(s, scaled(p.clip, sx, sy));
            Color c = p.color;
            c[3] *= alpha;
            wlr_scene_shadow_set_color(s, c.data());
            break;
        }
        case Kind::Rect: {
            auto* r = wlr_scene_rect_from_node(p.node);
            wlr_scene_rect_set_size(r, w, h);
            wlr_scene_rect_set_corner_radii(r, scaled(p.corners, std::min(sx, sy)));
            wlr_scene_rect_set_clipped_region(r, scaled(p.clip, sx, sy));
            Color c = p.color;
            c[3] *= alpha;
            wlr_scene_rect_set_color(r, premultiplied(c).data());
            break;
        }
        }
    }
}

void Snapshot::warp(const std::function<FPoint(double, double)>& at, float alpha) {
    warped_ = true;
    std::vector<float> points;
    for (Part& p : parts_) {
        if (p.kind != Kind::Buffer) {
            wlr_scene_node_set_enabled(p.node, false);
            continue;
        }
        const int cols = std::max(1, int(std::ceil(p.box.width / kGenieCell)));
        const int rows = std::max(1, int(std::ceil(p.box.height / kGenieCell)));
        points.resize(size_t(cols + 1) * (rows + 1) * 2);
        double x0 = INFINITY, y0 = INFINITY, x1 = -INFINITY, y1 = -INFINITY;
        for (int j = 0; j <= rows; j++)
            for (int i = 0; i <= cols; i++) {
                const FPoint q = at(p.box.x + p.box.width * i / cols, p.box.y + p.box.height * j / rows);
                const size_t k = size_t(j) * (cols + 1) + i;
                points[k * 2] = float(q.x);
                points[k * 2 + 1] = float(q.y);
                x0 = std::min(x0, q.x);
                y0 = std::min(y0, q.y);
                x1 = std::max(x1, q.x);
                y1 = std::max(y1, q.y);
            }
        // The node covers what's drawn (what gets damaged), the points
        // relative to it.
        const int nx = int(std::floor(x0)), ny = int(std::floor(y0));
        for (size_t k = 0; k < points.size(); k += 2) {
            points[k] -= float(nx);
            points[k + 1] -= float(ny);
        }
        auto* buf = wlr_scene_buffer_from_node(p.node);
        wlr_scene_node_set_position(p.node, frame_.x + nx, frame_.y + ny);
        wlr_scene_buffer_set_dest_size(buf, std::max(1, int(std::ceil(x1)) - nx), std::max(1, int(std::ceil(y1)) - ny));
        wlr_scene_buffer_set_opacity(buf, p.opacity * alpha);
        wlr_scene_buffer_set_corner_radii(buf, p.corners);
        wlr_scene_buffer_set_warp(buf, cols, rows, points.data(), round(p.box.width), round(p.box.height));
    }
}

} // namespace atrium
