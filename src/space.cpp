#include "space.hpp"

#include "output.hpp"
#include "server.hpp"
#include "snap_preview.hpp"
#include "view.hpp"

#include <algorithm>

namespace atrium {

namespace {

constexpr uint32_t kWorkspaceCaps =
    EXT_WORKSPACE_HANDLE_V1_WORKSPACE_CAPABILITIES_ACTIVATE;

} // namespace

Space::Space(Server& srv, Output* out, int n)
    : server(srv), output(out), number(n), secret(false) {
    tree = scene::Tree::create(server.layer(Layer::Views));
    fullscreen_tree = scene::Tree::create(server.layer(Layer::Fullscreen));
    tree->set_enabled(false);
    fullscreen_tree->set_enabled(false);

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
    tree = scene::Tree::create(server.layer(Layer::Secret));
    // The dimmed screen behind a secret space; clicking it puts the space
    // away. Dimmed only, not blurred, as caelestia's special workspaces are
    // (Hyprland's dim_special): the space's windows stand out plainly.
    const Color& dim = server.config.secret_backdrop;
    backdrop = scene::Rect::create(tree, 0, 0, premultiplied(dim).data());
    backdrop->data = this;
    fullscreen_tree = scene::Tree::create(tree);
    tree->set_enabled(false);

    handle = wlr_ext_workspace_handle_v1_create(server.workspace_manager, id().c_str(), kWorkspaceCaps |
        EXT_WORKSPACE_HANDLE_V1_WORKSPACE_CAPABILITIES_DEACTIVATE);
    wlr_ext_workspace_handle_v1_set_name(handle, label().c_str());
    wlr_ext_workspace_handle_v1_set_hidden(handle, true);
    handle->data = this;
}

Space::~Space() {
    server.animator.cancel_owner(this, false);
    if (server.snap_preview)
        server.snap_preview->rescue(this);
    // A switch sliding this space in or out belongs to its output; finish it
    // while both spaces still exist.
    if (output && !secret)
        server.animator.cancel_owner(output, true);
    if (handle) {
        handle->data = nullptr;
        wlr_ext_workspace_handle_v1_destroy(handle);
    }
    if (!secret)
        fullscreen_tree->destroy();
    tree->destroy();  // a secret space's fullscreen tree goes with it
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
        tree->set_enabled(shown);
        if (!secret)
            fullscreen_tree->set_enabled(shown);
    }
    if (handle)
        wlr_ext_workspace_handle_v1_set_active(handle, shown);
}

void Space::hide_now() {
    if (shown_)
        return;
    set_offset(0, 0);
    tree->set_enabled(false);
    if (!secret)
        fullscreen_tree->set_enabled(false);
}

void Space::set_offset(int dx, int dy) {
    tree->set_position(dx, dy);
    if (!secret)
        fullscreen_tree->set_position(dx, dy);
    // A secret backdrop covers the screen whatever the windows are doing.
    if (backdrop && output) {
        backdrop->set_position(output->box.x - dx, output->box.y - dy);
    }
}

void Space::ensure_tile_backdrop() {
    if (!output)
        return;
    if (!tile_dim) {
        tile_blur = scene::Blur::create(tree, 0, 0);
        tile_blur->set_use_cache(false);
        tile_dim = scene::Rect::create(tree, 0, 0, premultiplied(server.config.secret_backdrop).data());
        tile_dim->accepts_input = false;
        tile_dim->lower_to_bottom();
        tile_blur->lower_to_bottom();
    }
    // Relative to the space's tree, which sits at 0,0 in layout coordinates.
    tile_dim->set_position(output->box.x, output->box.y);
    tile_dim->set_size(output->box.width, output->box.height);
    tile_blur->set_position(output->box.x, output->box.y);
    tile_blur->set_size(output->box.width, output->box.height);
}

void Space::attach(Output* out) {
    output = out;
    if (!backdrop || !out)
        return;
    backdrop->set_position(out->box.x, out->box.y);
    backdrop->set_size(out->box.width, out->box.height);
    backdrop->set_color(premultiplied(server.config.secret_backdrop).data());
}

} // namespace atrium
