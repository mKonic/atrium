#include "space.hpp"

#include "output.hpp"
#include "server.hpp"
#include "view.hpp"

#include <algorithm>

namespace atrium {

namespace {

constexpr uint32_t kWorkspaceCaps =
    EXT_WORKSPACE_HANDLE_V1_WORKSPACE_CAPABILITIES_ACTIVATE;

} // namespace

Space::Space(Server& srv, Output* out, int n)
    : server(srv), output(out), number(n), secret(false) {
    tree = wlr_scene_tree_create(server.layer(Layer::Views));
    fullscreen_tree = wlr_scene_tree_create(server.layer(Layer::Fullscreen));
    wlr_scene_node_set_enabled(&tree->node, false);
    wlr_scene_node_set_enabled(&fullscreen_tree->node, false);

    handle = wlr_ext_workspace_handle_v1_create(server.workspace_manager, id().c_str(), kWorkspaceCaps);
    wlr_ext_workspace_handle_v1_set_name(handle, label().c_str());
    const uint32_t coord = uint32_t(n);
    wlr_ext_workspace_handle_v1_set_coordinates(handle, &coord, 1);
    if (output && output->workspace_group)
        wlr_ext_workspace_handle_v1_set_group(handle, output->workspace_group);
    handle->data = this;
}

Space::Space(Server& srv, std::string n)
    : server(srv), output(nullptr), number(0), name(std::move(n)), secret(true) {
    tree = wlr_scene_tree_create(server.layer(Layer::Secret));
    // The dimmed screen behind a secret space; clicking it puts the space away.
    const Color& dim = server.config.secret_backdrop;
    backdrop = wlr_scene_rect_create(tree, 0, 0, dim.data());
    backdrop->node.data = this;
    fullscreen_tree = wlr_scene_tree_create(tree);
    wlr_scene_node_set_enabled(&tree->node, false);

    handle = wlr_ext_workspace_handle_v1_create(server.workspace_manager, id().c_str(), kWorkspaceCaps |
        EXT_WORKSPACE_HANDLE_V1_WORKSPACE_CAPABILITIES_DEACTIVATE);
    wlr_ext_workspace_handle_v1_set_name(handle, label().c_str());
    wlr_ext_workspace_handle_v1_set_hidden(handle, true);
    handle->data = this;
}

Space::~Space() {
    if (handle) {
        handle->data = nullptr;
        wlr_ext_workspace_handle_v1_destroy(handle);
    }
    if (!secret)
        wlr_scene_node_destroy(&fullscreen_tree->node);
    wlr_scene_node_destroy(&tree->node);  // a secret space's fullscreen tree goes with it
}

std::string Space::id() const {
    if (secret)
        return "secret:" + name;
    return std::string(output ? output->wlr->name : "?") + ":" + std::to_string(number);
}

std::string Space::label() const {
    return secret ? name : std::to_string(number);
}

bool Space::empty() const {
    return std::ranges::none_of(server.views, [this](View* v) { return v->space == this; });
}

void Space::set_shown(bool shown) {
    shown_ = shown;
    wlr_scene_node_set_enabled(&tree->node, shown);
    if (!secret)
        wlr_scene_node_set_enabled(&fullscreen_tree->node, shown);
    if (handle)
        wlr_ext_workspace_handle_v1_set_active(handle, shown);
}

void Space::attach(Output* out) {
    output = out;
    if (!backdrop || !out)
        return;
    wlr_scene_node_set_position(&backdrop->node, out->box.x, out->box.y);
    wlr_scene_rect_set_size(backdrop, out->box.width, out->box.height);
    wlr_scene_rect_set_color(backdrop, server.config.secret_backdrop.data());
}

} // namespace atrium
