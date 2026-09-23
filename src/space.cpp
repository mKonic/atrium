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
    backdrop_blur = wlr_scene_blur_create(tree, 0, 0);
    wlr_scene_blur_set_should_only_blur_bottom_layer(backdrop_blur, false);  // blur the windows too
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
    server.animator.cancel_owner(this, false);
    // A switch sliding this space in or out belongs to its output; finish it
    // while both spaces still exist.
    if (output && !secret)
        server.animator.cancel_owner(output, true);
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

void Space::set_shown(bool shown, bool linger) {
    shown_ = shown;
    if (shown || !linger) {
        wlr_scene_node_set_enabled(&tree->node, shown);
        if (!secret)
            wlr_scene_node_set_enabled(&fullscreen_tree->node, shown);
    }
    if (handle)
        wlr_ext_workspace_handle_v1_set_active(handle, shown);
}

void Space::hide_now() {
    if (shown_)
        return;
    set_offset(0, 0);
    wlr_scene_node_set_enabled(&tree->node, false);
    if (!secret)
        wlr_scene_node_set_enabled(&fullscreen_tree->node, false);
}

void Space::set_offset(int dx, int dy) {
    wlr_scene_node_set_position(&tree->node, dx, dy);
    if (!secret)
        wlr_scene_node_set_position(&fullscreen_tree->node, dx, dy);
    // The backdrop covers the screen whatever the windows are doing.
    if (backdrop && output) {
        wlr_scene_node_set_position(&backdrop->node, output->box.x - dx, output->box.y - dy);
        wlr_scene_node_set_position(&backdrop_blur->node, output->box.x - dx, output->box.y - dy);
    }
}

void Space::attach(Output* out) {
    output = out;
    if (!backdrop || !out)
        return;
    wlr_scene_node_set_position(&backdrop->node, out->box.x, out->box.y);
    wlr_scene_rect_set_size(backdrop, out->box.width, out->box.height);
    wlr_scene_node_set_position(&backdrop_blur->node, out->box.x, out->box.y);
    wlr_scene_blur_set_size(backdrop_blur, out->box.width, out->box.height);
    wlr_scene_node_set_enabled(&backdrop_blur->node, server.config.blur);
    wlr_scene_rect_set_color(backdrop, server.config.secret_backdrop.data());
}

} // namespace atrium
