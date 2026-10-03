#include "dump.hpp"

#include "scene.hpp"

#include <cmath>

namespace atrium::scene {

namespace {

const char* type_name(Type t) {
    switch (t) {
    case Type::Tree: return "tree";
    case Type::Rect: return "rect";
    case Type::Buffer: return "buffer";
    case Type::Shadow: return "shadow";
    case Type::BlurCache: return "blur_cache";
    case Type::Blur: return "blur";
    }
    return "?";
}

double round2(double v) {
    return std::round(v * 100) / 100;
}

} // namespace

nlohmann::json dump(const Node* node) {
    using json = nlohmann::json;
    const Placement p = node->placement();
    int w = 0, h = 0;
    node->size(&w, &h);
    json j{{"type", type_name(node->type)},
           {"enabled", node->enabled},
           {"shown", p.enabled},
           {"x", round2(p.x)},
           {"y", round2(p.y)},
           {"width", round2(w * p.scale)},
           {"height", round2(h * p.scale)},
           {"opacity", round2(p.opacity)}};
    switch (node->type) {
    case Type::Tree: {
        const Tree* t = static_cast<const Tree*>(node);
        j["opacity"] = round2(p.opacity * t->opacity());  // what it and its children are drawn at
        j["scale"] = round2(t->scale());
        j["warped"] = bool(t->warp());
        j["moving"] = t->moving();
        json children = json::array();
        for (Node* c : each_child(t))
            children.push_back(dump(c));
        j["children"] = std::move(children);
        break;
    }
    case Type::Blur: {
        const Blur* b = static_cast<const Blur*>(node);
        j["strength"] = round2(b->strength);
        j["alpha"] = round2(b->alpha);
        j["source"] = b->use_cache ? "desktop" : "below";
        break;
    }
    case Type::Rect: {
        const Rect* r = static_cast<const Rect*>(node);
        j["color"] = {round2(r->color[0]), round2(r->color[1]), round2(r->color[2]), round2(r->color[3])};
        break;
    }
    default:
        break;
    }
    return j;
}

} // namespace atrium::scene
