#include "scene_dump.hpp"

#include <cmath>

namespace atrium {

namespace {

double round2(double v) {
    return std::round(v * 100) / 100;
}

bool shown(const wlr_scene_node* node) {
    for (; node; node = node->parent ? &node->parent->node : nullptr)
        if (!node->enabled)
            return false;
    return true;
}

} // namespace

nlohmann::json dump_scene(wlr_scene_node* node) {
    using json = nlohmann::json;
    int x = 0, y = 0;
    wlr_scene_node_coords(node, &x, &y);
    json j{{"enabled", node->enabled}, {"shown", shown(node)}, {"x", x}, {"y", y}};
    auto size = [&](int w, int h) {
        j["width"] = w;
        j["height"] = h;
    };
    switch (node->type) {
    case WLR_SCENE_NODE_TREE: {
        j["type"] = "tree";
        json children = json::array();
        wlr_scene_node* child;
        wl_list_for_each(child, &wlr_scene_tree_from_node(node)->children, link)
            children.push_back(dump_scene(child));
        j["children"] = std::move(children);
        break;
    }
    case WLR_SCENE_NODE_RECT: {
        const auto* r = wlr_scene_rect_from_node(node);
        j["type"] = "rect";
        size(r->width, r->height);
        j["color"] = {round2(r->color[0]), round2(r->color[1]), round2(r->color[2]), round2(r->color[3])};
        break;
    }
    case WLR_SCENE_NODE_BUFFER: {
        const auto* b = wlr_scene_buffer_from_node(node);
        j["type"] = "buffer";
        const int w = b->dst_width > 0 ? b->dst_width : b->buffer ? b->buffer->width : 0;
        const int h = b->dst_height > 0 ? b->dst_height : b->buffer ? b->buffer->height : 0;
        size(w, h);
        j["opacity"] = round2(b->opacity);
        j["has_buffer"] = b->buffer != nullptr;
        break;
    }
    case WLR_SCENE_NODE_SHADOW: {
        const auto* s = wlr_scene_shadow_from_node(node);
        j["type"] = "shadow";
        size(s->width, s->height);
        j["sigma"] = round2(s->blur_sigma);
        break;
    }
    case WLR_SCENE_NODE_OPTIMIZED_BLUR:
        j["type"] = "blur_cache";
        break;
    case WLR_SCENE_NODE_BLUR: {
        const auto* b = wlr_scene_blur_from_node(node);
        j["type"] = "blur";
        size(b->width, b->height);
        j["strength"] = round2(b->strength);
        j["alpha"] = round2(b->alpha);
        // The desktop's blur, made once (wallpaper and bottom layers), or
        // whatever is below it, live.
        j["source"] = b->should_only_blur_bottom_layer ? "desktop" : "below";
        break;
    }
    }
    return j;
}

} // namespace atrium
